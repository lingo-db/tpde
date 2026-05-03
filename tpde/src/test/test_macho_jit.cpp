// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Smallest possible end-to-end exercise of the macOS / arm64 JIT path:
// build an `add(a,b) { ret a+b; }` via the test IR adaptor, swap the
// default ELF assembler for `AssemblerMachOA64`, hand the result to
// `MachOMapper`, then call the resulting function pointer.
//
// Intentionally a separate executable from `tpde_test` so the wiring
// (PlatformConfigDarwin, Mach-O assembler, MAP_JIT mapper) is exercised in
// isolation without touching the existing ELF-LIT test infrastructure.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <unwind.h>

#include "TestIR.hpp"
#include "TestIRAdaptor.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/MachOMapper.hpp"
#include "tpde/arm64/CompilerA64.hpp"

// Diagnostic accessor exposed by `MachOMapper.cpp` so the EH test can
// confirm that libunwind's dynamic-section callback is actually being
// consulted (vs. backtrace silently falling back to FP-chain walking,
// which would let it traverse JIT'd frames even without registration
// and mask a broken integration).
namespace tpde::macho {
u64 tpde_macho_jit_unwind_hits();
}

namespace {
using namespace tpde;
using namespace tpde::test;

struct TestIRCompilerA64Darwin
    : a64::CompilerA64<TestIRAdaptor,
                       TestIRCompilerA64Darwin,
                       CompilerBase,
                       a64::PlatformConfigDarwin> {
  using Base = a64::CompilerA64<TestIRAdaptor,
                                TestIRCompilerA64Darwin,
                                CompilerBase,
                                a64::PlatformConfigDarwin>;

  explicit TestIRCompilerA64Darwin(TestIRAdaptor *adaptor) : Base{adaptor} {}

  // No per-function personality. The test JIT'd frames are
  // pass-through for C++ exceptions (no catch of their own). For
  // those, libunwind walks based on MODE_FRAME / MODE_FRAMELESS
  // alone — same as what clang emits for an `extern "C" void f()
  // { thrower(); }` trampoline.
  SymRef cur_personality_func() const { return {}; }

  bool cur_func_may_emit_calls() const {
    return this->ir()->functions[this->adaptor->cur_func].has_call;
  }

  struct ValueParts {
    static u32 count() { return 1; }
    static u32 size_bytes(u32) { return 8; }
    static RegBank reg_bank(u32) { return a64::PlatformConfigDarwin::GP_BANK; }
  };
  ValueParts val_parts(IRValueRef) { return ValueParts{}; }

  AsmReg select_fixed_assignment_reg(AssignmentPartRef ap,
                                     const IRValueRef value) {
    return Base::select_fixed_assignment_reg(ap, value);
  }
  // Honor the test IR's `%a!` syntax so we can construct cases that
  // genuinely exercise callee-save STPs (and therefore the compact
  // unwind bitmap).
  bool try_force_fixed_assignment(const IRValueRef value) const {
    return ir()->values[static_cast<u32>(value)].force_fixed_assignment;
  }

  std::optional<ValRefSpecial> val_ref_special(IRValueRef) { return {}; }
  ValuePart val_part_ref_special(ValRefSpecial &, u32) {
    TPDE_UNREACHABLE("val_part_ref_special on IR without special values");
  }

  void define_func_idx(IRFuncRef func, const u32 idx) {
    assert(static_cast<u32>(func) == idx);
    (void)func;
    (void)idx;
  }

  TestIR *ir() { return this->adaptor->ir; }
  const TestIR *ir() const { return this->adaptor->ir; }

  // Subset of TestIRCompilerA64::compile_inst — only the cases reached by an
  // `add`+`ret`(+optional `call`) IR. Intentionally not pulling in the full
  // opcode table so this stub stays self-contained.
  [[nodiscard]] bool compile_inst(IRInstRef inst_idx, InstRange) {
    const TestIR::Value &value = ir()->values[static_cast<u32>(inst_idx)];

    using Op = TestIR::Value::Op;
    switch (value.op) {
    case Op::add: {
      const auto lhs_idx = static_cast<IRValueRef>(
          ir()->value_operands[value.op_begin_idx]);
      const auto rhs_idx = static_cast<IRValueRef>(
          ir()->value_operands[value.op_begin_idx + 1]);
      auto [lhs_vr, lhs] = this->val_ref_single(lhs_idx);
      auto [rhs_vr, rhs] = this->val_ref_single(rhs_idx);
      auto [res_vr, res] =
          this->result_ref_single(static_cast<IRValueRef>(inst_idx));
      AsmReg lhs_reg = lhs.load_to_reg();
      AsmReg rhs_reg = rhs.load_to_reg();
      AsmReg res_reg = res.alloc_try_reuse(lhs);
      ASM(ADDx, res_reg, lhs_reg, rhs_reg);
      res.set_modified();
      return true;
    }
    case Op::call: {
      const auto func_idx = value.call_func_idx;
      auto operands = std::span<IRValueRef>{
          reinterpret_cast<IRValueRef *>(ir()->value_operands.data() +
                                         value.op_begin_idx),
          value.op_count};
      auto res_ref = this->result_ref(static_cast<IRValueRef>(inst_idx));
      util::SmallVector<CallArg, 8> arguments;
      for (auto op : operands) {
        arguments.push_back(CallArg{op});
      }
      this->generate_call(this->func_syms[func_idx], arguments, &res_ref);
      return true;
    }
    case Op::terminate:
    case Op::ret: {
      RetBuilder rb{*this, *cur_cc_assigner()};
      if (value.op_count == 1) {
        const auto op = static_cast<IRValueRef>(
            this->adaptor->ir->value_operands[value.op_begin_idx]);
        rb.add(op);
      }
      rb.ret();
      return true;
    }
    default: return false;
    }
  }
};

} // namespace

// Host-side C function we'll resolve from JIT'd code below to exercise
// the cross-DSO call path. Marked `extern "C"` so the JIT can request it
// by an unmangled name. Marked `__attribute__((used))` so even at -O3
// the compiler doesn't optimize it away as unreachable from main().
extern "C" __attribute__((used)) u64 host_doubler(u64 x) { return x * 2; }

// Sentinel type carrying a magic value, thrown by `host_thrower`
// below. Using a type instead of a raw `int` so the catch-clause is
// unambiguously the one we expect (catching `int` accidentally
// matches a wider class of throws).
struct UnwindProbe {
  u64 magic;
};

// Throws unconditionally. JIT'd code calls this transitively through
// a JIT trampoline, with the host-side test harness wrapping the
// trampoline call in `try { ... } catch (UnwindProbe &)`. For the
// catch to fire, the C++ runtime / libunwind must:
//   1. Walk *out* of host_thrower's frame (host has unwind info).
//   2. Walk *through* the JIT trampoline frame — using the
//      compact-unwind record that `MachOMapper` registered with
//      libunwind via `__unw_add_find_dynamic_unwind_sections`.
//   3. Land in main's `catch` clause.
//
// If step 2 is broken (encoding wrong, registration missed, dso_base
// fix-up off, ...), libunwind either aborts the process or restores
// garbage register state — both of which the test harness would
// surface as a failure rather than a clean catch.
extern "C" __attribute__((noinline, used, noreturn)) void host_thrower() {
  throw UnwindProbe{0xfeed'face'cafe'beefULL};
}

// Walk via `_Unwind_Backtrace`, which on macOS goes through libunwind
// (and therefore consults the dynamic-section callbacks our
// `MachOMapper` registers). `backtrace()` from <execinfo.h> would NOT
// — Apple's libsystem_c `backtrace()` is a plain FP-chain walker that
// doesn't ask libunwind for anything.
struct WalkState {
  int count = 0;
};
static _Unwind_Reason_Code unwind_count_cb(struct _Unwind_Context *ctx,
                                           void *arg) {
  auto *st = static_cast<WalkState *>(arg);
  if (_Unwind_GetIP(ctx) == 0) {
    return _URC_END_OF_STACK;
  }
  st->count++;
  return _URC_NO_REASON;
}

extern "C" __attribute__((noinline, used)) u64 host_walk_stack(u64 sentinel) {
  WalkState st;
  _Unwind_Backtrace(unwind_count_cb, &st);
  return (u64(uint32_t(st.count)) << 32) | uint32_t(sentinel);
}

__attribute__((noinline)) static int native_walk_count() {
  WalkState st;
  _Unwind_Backtrace(unwind_count_cb, &st);
  return st.count;
}

// Compile, JIT-link, and call a TPDE test-IR snippet. Returns the named
// function as a typed function pointer; the mapper is *retained* in `out`
// so the JIT pages stay mapped for the duration of the call.
template <typename Fn>
static Fn jit_compile(const char *src,
                      std::string_view fn_name,
                      macho::MachOMapper &out) {
  TestIR ir;
  if (!ir.parse_ir(src)) {
    std::fprintf(stderr, "Failed to parse IR:\n%s\n", src);
    std::exit(1);
  }
  TestIRAdaptor adaptor{&ir};
  TestIRCompilerA64Darwin compiler{&adaptor};
  if (!compiler.compile()) {
    std::fprintf(stderr, "Failed to compile IR\n");
    std::exit(1);
  }
  bool ok = out.map(compiler.assembler,
                    [](std::string_view name) -> void * {
                      // Host-side functions exposed for the cross-DSO test.
                      if (name == "host_doubler") {
                        return reinterpret_cast<void *>(&host_doubler);
                      }
                      if (name == "host_walk_stack") {
                        return reinterpret_cast<void *>(&host_walk_stack);
                      }
                      if (name == "host_thrower") {
                        return reinterpret_cast<void *>(&host_thrower);
                      }
                      // Names like `__gxx_personality_v0` come from
                      // libc++abi — return nullptr so the mapper
                      // falls back to dlsym(RTLD_DEFAULT, ...). Print
                      // anything *else* unexpected.
                      if (name == "__gxx_personality_v0") {
                        return nullptr;
                      }
                      std::fprintf(stderr,
                                   "test: unexpected resolver call '%.*s'\n",
                                   int(name.size()), name.data());
                      return nullptr;
                    });
  if (!ok) {
    std::fprintf(stderr, "MachOMapper::map failed\n");
    std::exit(1);
  }

  // Locate the function by name in the IR's function list, then look up the
  // matching SymRef from `func_syms` (parallel-indexed).
  for (size_t i = 0; i < ir.functions.size(); ++i) {
    if (ir.functions[i].name == fn_name && !ir.functions[i].declaration) {
      void *p = out.get_sym_addr(compiler.func_syms[i]);
      if (!p) {
        std::fprintf(stderr, "Failed to resolve %.*s\n",
                     int(fn_name.size()), fn_name.data());
        std::exit(1);
      }
      return reinterpret_cast<Fn>(p);
    }
  }
  std::fprintf(stderr, "Function '%.*s' not found in IR\n",
               int(fn_name.size()), fn_name.data());
  std::exit(1);
}

int main(int argc, char *argv[]) {
  using AddFn = u64 (*)(u64, u64);

  // ----- Sanity probe: does the host's _Unwind_Backtrace work at all? -----
  {
    int n = native_walk_count();
    std::printf("native_walk_count = %d\n", n);
  }

  // Optional `--obj-out <path> [<src-file>]` mode: emit a Mach-O .o for
  // either the default `add(a,b)` IR or an IR file the user supplies.
  // Useful as an end-to-end check of the object-file writer (the JIT
  // path doesn't go through it).
  if ((argc == 3 || argc == 4) &&
      std::string_view(argv[1]) == "--obj-out") {
    std::string src_buf =
        "define @add(%a, %b) {\n"
        "entry:\n"
        "  %res = add %a, %b\n"
        "  ret %res\n"
        "}\n";
    if (argc == 4) {
      FILE *f = std::fopen(argv[3], "rb");
      if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[3]);
        return 1;
      }
      src_buf.clear();
      char buf[4096];
      while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
        src_buf.append(buf, n);
      }
      std::fclose(f);
    }
    TestIR ir;
    if (!ir.parse_ir(src_buf)) {
      std::fprintf(stderr, "Failed to parse IR\n");
      return 1;
    }
    TestIRAdaptor adaptor{&ir};
    TestIRCompilerA64Darwin compiler{&adaptor};
    if (!compiler.compile()) {
      std::fprintf(stderr, "Failed to compile IR\n");
      return 1;
    }
    auto bytes = compiler.assembler.build_object_file();
    FILE *f = std::fopen(argv[2], "wb");
    if (!f) {
      std::fprintf(stderr, "cannot open %s\n", argv[2]);
      return 1;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return 0;
  }

  // ----- Case 1: leaf `add` --------------------------------------------------
  {
    macho::MachOMapper mapper;
    auto add = jit_compile<AddFn>(
        "define @add(%a, %b) {\n"
        "entry:\n"
        "  %res = add %a, %b\n"
        "  ret %res\n"
        "}\n",
        "add", mapper);

    u64 r = add(2, 3);
    std::printf("add(2,3) = %llu\n", static_cast<unsigned long long>(r));
    if (r != 5) {
      std::fprintf(stderr, "FAIL: expected 5, got %llu\n",
                   static_cast<unsigned long long>(r));
      return 1;
    }
    if (add(0xdeadbeefULL, 0x1000ULL) != 0xdeadbeefULL + 0x1000ULL) {
      std::fprintf(stderr, "FAIL: large add\n");
      return 1;
    }
  }

  // ----- Case 2: cross-DSO call to a host C function -----------------------
  // Exercises the JIT mapper's symbol resolver + dynamic loader path:
  // calls `host_doubler` (defined in this binary) from JIT'd code, which
  // forces the mapper to (a) recognize an undefined external, (b) resolve
  // it via the resolver callback, (c) install a PLT trampoline since the
  // host symbol is potentially outside ±128 MiB BRANCH26 range. Same
  // calling-convention pathway darwinpcs uses for libSystem calls.
  {
    macho::MachOMapper mapper;
    auto fn = jit_compile<u64 (*)(u64)>(
        "declare @host_doubler(%a)\n"
        "define @bridge(%x) {\n"
        "entry:\n"
        "  %r = call @host_doubler, %x\n"
        "  ret %r\n"
        "}\n",
        "bridge", mapper);
    u64 r = fn(21);
    std::printf("bridge(21) = %llu\n", static_cast<unsigned long long>(r));
    if (r != 42) {
      std::fprintf(stderr, "FAIL: expected 42, got %llu\n",
                   static_cast<unsigned long long>(r));
      return 1;
    }
  }

  // ----- Case 3: stack walk through a JIT'd frame --------------------------
  // Validates the M4 libunwind dynamic-registration plumbing. We JIT a
  // function `walker(x)` that calls `host_walk_stack(x)` from the host;
  // host_walk_stack walks the stack via `backtrace()`. For the unwinder
  // to step past the JIT'd frame back into main() it must discover the
  // JIT region's compact-unwind entry through the dynamic callback we
  // registered in `MachOMapper::map`.
  //
  // Backtrace can technically also walk an FP chain on AArch64 without
  // any unwind info, so frame count alone isn't a strict proof. The
  // strong signal is the diagnostic counter
  // `tpde_macho_jit_unwind_hits` incremented inside our find-callback
  // — it's only nonzero if libunwind actually called us back.
  {
    using namespace tpde::macho;
    u64 hits_before = tpde_macho_jit_unwind_hits();
    macho::MachOMapper mapper;
    auto walker = jit_compile<u64 (*)(u64)>(
        "declare @host_walk_stack(%a)\n"
        "define @walker(%x) {\n"
        "entry:\n"
        "  %r = call @host_walk_stack, %x\n"
        "  ret %r\n"
        "}\n",
        "walker", mapper);
    u64 r = walker(0xfeedface);
    u64 hits_after = tpde_macho_jit_unwind_hits();
    u32 frame_count = u32(r >> 32);
    u32 sentinel = u32(r);
    std::printf(
        "walker(0xfeedface) -> frames=%u sentinel=0x%x cb_hits=%llu\n",
        frame_count, sentinel,
        static_cast<unsigned long long>(hits_after - hits_before));
    if (sentinel != 0xfeedface) {
      std::fprintf(stderr, "FAIL: sentinel mismatch\n");
      return 1;
    }
    // Strong signal: did libunwind ASK us about the JIT'd PC? If yes,
    // the registration is in place and the callback's lookup table
    // covers the JIT region. The actual frame count from
    // `_Unwind_Backtrace` is a weaker signal (it depends on how
    // libunwind decides to step through our compact-unwind encoding,
    // which is exercised more thoroughly by EH end-to-end tests
    // queued for M4 follow-up).
    if (hits_after == hits_before) {
      std::fprintf(stderr,
                   "FAIL: libunwind never consulted the dynamic-section "
                   "callback — JIT compact-unwind registration not wired\n");
      return 1;
    }
  }

  // ----- Case 4: intra-module call (exercises ARM64_RELOC_BRANCH26) ---------
  // `caller(x) -> add(x, x) + x` — caller invokes add, both live in the same
  // JIT region. The branch fits easily within 128 MiB so no PLT trampoline
  // is needed; the BRANCH26 relocation is resolved in-place.
  {
    macho::MachOMapper mapper;
    auto caller = jit_compile<u64 (*)(u64)>(
        "define @add(%a, %b) {\n"
        "entry:\n"
        "  %res = add %a, %b\n"
        "  ret %res\n"
        "}\n"
        "define @caller(%x) {\n"
        "entry:\n"
        "  %doubled = call @add, %x, %x\n"
        "  %r = add %doubled, %x\n"
        "  ret %r\n"
        "}\n",
        "caller", mapper);
    u64 r = caller(7);
    std::printf("caller(7) = %llu\n", static_cast<unsigned long long>(r));
    if (r != 21) {
      std::fprintf(stderr, "FAIL: expected 21, got %llu\n",
                   static_cast<unsigned long long>(r));
      return 1;
    }
  }

  // ----- Sanity: host->host exception works at all in this build ---------
  {
    bool ok = false;
    try {
      host_thrower();
    } catch (UnwindProbe &p) {
      ok = (p.magic == 0xfeed'face'cafe'beefULL);
    } catch (...) {
    }
    std::printf("eh-host-only: caught=%d\n", int(ok));
    if (!ok) {
      std::fprintf(stderr,
                   "FAIL: exceptions don't even work without JIT — "
                   "build flags wrong\n");
      return 1;
    }
  }

  // ----- Case 5: C++ exception unwinding through a JIT'd frame -------------
  //
  // The strongest validation of the M4 stack. For the `catch` to fire,
  // Apple's libunwind must:
  //   - Step out of host_thrower (host frame; has unwind info).
  //   - Find unwind info for the JIT'd trampoline via the dynamic
  //     callback we registered.
  //   - Decode the eh_frame FDE well enough to restore caller state.
  //   - Land in main's catch handler here.
  //
  // The crucial bit on Apple: libunwind's dynamic-section path for
  // *exception unwinding* falls back to eh_frame when its compact-
  // unwind lookup doesn't reconstruct a valid handler chain (which
  // happens here because we register raw `__LD,__compact_unwind`
  // records, not the hierarchical `__TEXT,__unwind_info` format
  // `ld` would synthesize). Wiring the eh_frame section through
  // the `dwarf_section` field of `unw_dynamic_unwind_sections` lets
  // libunwind walk our pre-existing FDE data and unwind succeeds.
  //
  // For backtrace-style walking (`_Unwind_Backtrace` in case 3)
  // compact-unwind alone is enough; for C++ EH, eh_frame is the
  // load-bearing path until proper `__unwind_info` synthesis lands.
  {
    macho::MachOMapper mapper;
    auto trampoline = jit_compile<u64 (*)()>(
        "declare @host_thrower()\n"
        "define @jit_trampoline() {\n"
        "entry:\n"
        "  %r = call @host_thrower\n"
        "  ret %r\n"
        "}\n",
        "jit_trampoline", mapper);
    bool caught = false;
    u64 caught_magic = 0;
    try {
      (void)trampoline();
      std::fprintf(stderr,
                   "FAIL: trampoline returned without throwing\n");
      return 1;
    } catch (UnwindProbe &p) {
      caught = true;
      caught_magic = p.magic;
    } catch (...) {
      std::fprintf(stderr,
                   "FAIL: caught wrong exception type\n");
      return 1;
    }
    std::printf("eh-jit: caught=%d magic=0x%llx\n", int(caught),
                static_cast<unsigned long long>(caught_magic));
    if (!caught || caught_magic != 0xfeed'face'cafe'beefULL) {
      std::fprintf(stderr,
                   "FAIL: exception did not propagate cleanly through "
                   "JIT'd frame\n");
      return 1;
    }
  }

  std::printf("OK\n");
  return 0;
}
