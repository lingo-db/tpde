// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native core-backend checks, independent of the LLVM frontend.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unwind.h>

#include "TestIR.hpp"
#include "TestIRAdaptor.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/MachOMapper.hpp"
#include "tpde/arm64/CompilerA64.hpp"
#include "tpde/arm64/PlatformConfigDarwin.hpp"

namespace {
using namespace tpde;
using namespace tpde::test;

struct TestIRCompilerA64Darwin : a64::CompilerA64<TestIRAdaptor,
                                                  TestIRCompilerA64Darwin,
                                                  CompilerBase,
                                                  a64::PlatformConfigDarwin> {
  using Base = a64::CompilerA64<TestIRAdaptor,
                                TestIRCompilerA64Darwin,
                                CompilerBase,
                                a64::PlatformConfigDarwin>;

  explicit TestIRCompilerA64Darwin(TestIRAdaptor *adaptor) : Base{adaptor} {}

  // These frames pass exceptions through without a handler of their own.
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

  // Force a callee-saved register so unwinding must restore its value.
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
      const auto lhs_idx =
          static_cast<IRValueRef>(ir()->value_operands[value.op_begin_idx]);
      const auto rhs_idx =
          static_cast<IRValueRef>(ir()->value_operands[value.op_begin_idx + 1]);
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

extern "C" __attribute__((used)) u64 host_doubler(u64 x) { return x * 2; }

struct UnwindProbe {
  u64 magic;
};

// The JIT bridge must restore its caller state when this exception escapes.
extern "C" __attribute__((noinline, used, noreturn)) void host_thrower() {
  throw UnwindProbe{0xfeed'face'cafe'beefULL};
}

// Exercise libunwind rather than a frame-pointer-only backtrace.
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

static void check(bool success, const char *expression) {
  if (!success) {
    std::fprintf(stderr, "FAIL: %s\n", expression);
    std::exit(1);
  }
}
#define CHECK(expr) check(bool(expr), #expr)

// The caller retains the mapper for as long as it uses the returned function.
template <typename Fn>
static Fn jit_compile(const char *src,
                      std::string_view name,
                      macho::MachOMapper &out) {
  TestIR ir;
  CHECK(ir.parse_ir(src));
  TestIRAdaptor adaptor{&ir};
  TestIRCompilerA64Darwin compiler{&adaptor};
  CHECK(compiler.compile());
  CHECK(out.map(compiler.assembler, [](std::string_view symbol) -> void * {
    if (symbol == "host_doubler") {
      return reinterpret_cast<void *>(&host_doubler);
    }
    if (symbol == "host_walk_stack") {
      return reinterpret_cast<void *>(&host_walk_stack);
    }
    if (symbol == "host_thrower") {
      return reinterpret_cast<void *>(&host_thrower);
    }
    return nullptr;
  }));
  for (size_t i = 0; i < ir.functions.size(); ++i) {
    if (ir.functions[i].name == name && !ir.functions[i].declaration) {
      void *address = out.get_sym_addr(compiler.func_syms[i]);
      CHECK(address);
      return reinterpret_cast<Fn>(address);
    }
  }
  CHECK(false && "function not found");
  return nullptr;
}

bool test_macho_assembler();

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--reject-vararg-helper") {
    TestIR ir;
    TestIRAdaptor adaptor{&ir};
    TestIRCompilerA64Darwin compiler{&adaptor};
    compiler.generate_call(SymRef{}, {}, nullptr, true);
    return 0;
  }
  if (argc != 1) {
    CHECK(argc == 4 && std::string_view(argv[1]) == "--obj-out");
    std::ifstream input(argv[3]);
    CHECK(input.is_open());
    std::string source{std::istreambuf_iterator<char>(input), {}};
    TestIR ir;
    CHECK(ir.parse_ir(source));
    TestIRAdaptor adaptor{&ir};
    TestIRCompilerA64Darwin compiler{&adaptor};
    CHECK(compiler.compile());
    auto bytes = compiler.assembler.build_object_file();
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    output.close();
    CHECK(output.good());
    return 0;
  }
  CHECK(test_macho_assembler());
  {
    macho::MachOMapper mapper;
    auto add = jit_compile<u64 (*)(u64, u64)>(R"(
define @add(%a, %b) {
entry:
  %r = add %a, %b
  ret %r
}
)",
                                              "add",
                                              mapper);
    CHECK(add(2, 3) == 5);
    CHECK(add(0xdeadbeefULL, 0x1000ULL) == 0xdeadbeefULL + 0x1000ULL);
  }
  {
    macho::MachOMapper mapper;
    auto bridge = jit_compile<u64 (*)(u64)>(R"(
declare @host_doubler(%a)
define @bridge(%x) {
entry:
  %r = call @host_doubler, %x
  ret %r
}
)",
                                            "bridge",
                                            mapper);
    CHECK(bridge(21) == 42);
  }
  {
    macho::MachOMapper mapper;
    auto walker = jit_compile<u64 (*)(u64)>(R"(
declare @host_walk_stack(%a)
define @walker(%x) {
entry:
  %r = call @host_walk_stack, %x
  ret %r
}
)",
                                            "walker",
                                            mapper);
    u64 result = walker(0xfeedface);
    CHECK(u32(result) == 0xfeedface);
    CHECK((result >> 32) >= 3);
  }
  {
    macho::MachOMapper mapper;
    auto caller = jit_compile<u64 (*)(u64)>(R"(
define @add(%a, %b) {
entry:
  %r = add %a, %b
  ret %r
}
define @caller(%x) {
entry:
  %doubled = call @add, %x, %x
  %r = add %doubled, %x
  ret %r
}
)",
                                            "caller",
                                            mapper);
    CHECK(caller(7) == 21);
  }
  // Check host exception support before unwinding through generated code.
  try {
    host_thrower();
  } catch (const UnwindProbe &p) {
    CHECK(p.magic == 0xfeed'face'cafe'beefULL);
  }

  macho::MachOMapper mapper;
  auto trampoline = jit_compile<u64 (*)(u64)>(R"(
declare @host_thrower()
define @jit_trampoline(%x!) {
entry:
  %r = call @host_thrower
  %sum = add %x, %r
  ret %sum
}
)",
                                              "jit_trampoline",
                                              mapper);
  register u64 sentinel asm("x19") = 0x123456789abcdef0ULL;
  asm volatile("" : "+r"(sentinel));
  try {
    (void)trampoline(55);
    CHECK(false && "trampoline returned without throwing");
  } catch (const UnwindProbe &p) {
    CHECK(p.magic == 0xfeed'face'cafe'beefULL);
  }
  asm volatile("" : "+r"(sentinel));
  CHECK(sentinel == 0x123456789abcdef0ULL);
  std::puts("OK");
}
