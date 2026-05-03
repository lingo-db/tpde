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

#include <cstdio>
#include <cstdlib>

#include "TestIR.hpp"
#include "TestIRAdaptor.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/MachOMapper.hpp"
#include "tpde/arm64/CompilerA64.hpp"

namespace {
using namespace tpde;
using namespace tpde::test;

// Mach-O / Darwin variant of the AArch64 platform config. Identical to the
// stock `a64::PlatformConfig` except for the `Assembler` typedef. The default
// `CCAssignerAAPCS` works for the trivial `add(a,b)` test — both AAPCS and
// darwinpcs put the first two integer args in `x0`/`x1` and the return value
// in `x0`, so register-only calls match. Variadic / large-aggregate cases
// will need a real `CCAssignerDarwinAArch64` (M3).
struct PlatformConfigDarwin : a64::PlatformConfig {
  using Assembler = tpde::macho::AssemblerMachOA64;
};

struct TestIRCompilerA64Darwin
    : a64::CompilerA64<TestIRAdaptor,
                       TestIRCompilerA64Darwin,
                       CompilerBase,
                       PlatformConfigDarwin> {
  using Base = a64::CompilerA64<TestIRAdaptor,
                                TestIRCompilerA64Darwin,
                                CompilerBase,
                                PlatformConfigDarwin>;

  explicit TestIRCompilerA64Darwin(TestIRAdaptor *adaptor) : Base{adaptor} {}

  SymRef cur_personality_func() const { return {}; }

  bool cur_func_may_emit_calls() const {
    return this->ir()->functions[this->adaptor->cur_func].has_call;
  }

  struct ValueParts {
    static u32 count() { return 1; }
    static u32 size_bytes(u32) { return 8; }
    static RegBank reg_bank(u32) { return PlatformConfigDarwin::GP_BANK; }
  };
  ValueParts val_parts(IRValueRef) { return ValueParts{}; }

  AsmReg select_fixed_assignment_reg(AssignmentPartRef ap,
                                     const IRValueRef value) {
    return Base::select_fixed_assignment_reg(ap, value);
  }
  bool try_force_fixed_assignment(const IRValueRef) const { return false; }

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

  // Optional `--obj-out <path>` mode: emit a Mach-O .o for `add(a,b)` that
  // can be linked with Apple `ld` / clang. Useful as an end-to-end check of
  // the object-file writer (the JIT path doesn't go through it).
  if (argc == 3 && std::string_view(argv[1]) == "--obj-out") {
    TestIR ir;
    const char *src =
        "define @add(%a, %b) {\n"
        "entry:\n"
        "  %res = add %a, %b\n"
        "  ret %res\n"
        "}\n";
    if (!ir.parse_ir(src)) {
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

  // ----- Case 2: intra-module call (exercises ARM64_RELOC_BRANCH26) ---------
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

  std::printf("OK\n");
  return 0;
}
