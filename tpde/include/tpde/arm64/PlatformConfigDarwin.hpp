// SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "tpde/AssemblerMachO.hpp"
#include "tpde/arm64/CompilerA64.hpp"

namespace tpde::a64 {

/// Apple's AArch64 ABI: x18 is reserved, named stack arguments are packed,
/// and unnamed arguments use eight-byte stack slots.
class CCAssignerDarwinAArch64 : public CCAssigner {
  static constexpr CCInfo Info{
      .allocatable_regs =
          AAPCS_CC_INFO.allocatable_regs & ~create_bitmask({AsmReg::R18}),
      .callee_saved_regs = AAPCS_CC_INFO.callee_saved_regs,
      .arg_regs = AAPCS_CC_INFO.arg_regs,
  };

  u32 ngrn = 0, nsrn = 0, nsaa = 0;
  u32 ret_ngrn = 0, ret_nsrn = 0;
  bool unnamed = false;

public:
  CCAssignerDarwinAArch64() : CCAssigner(Info) {}

  void reset() override {
    ngrn = nsrn = nsaa = ret_ngrn = ret_nsrn = 0;
    unnamed = false;
  }

  void start_varargs() override {
    unnamed = true;
    nsaa = util::align_up(nsaa, 8);
  }

  void assign_arg(CCAssignment &arg) override {
    // Byval and sret behave like AAPCS — pointer in x8 / pointer on
    // stack. Same wire convention.
    if (arg.byval) [[unlikely]] {
      nsaa = util::align_up(nsaa, arg.align < 8 ? 8 : arg.align);
      arg.stack_off = nsaa;
      nsaa += arg.size;
      return;
    }
    if (arg.sret) [[unlikely]] {
      arg.reg = AsmReg{AsmReg::R8};
      return;
    }

    u32 &next = arg.bank == RegBank{0} ? ngrn : nsrn;
    if (!unnamed && next + arg.consecutive < 8) {
      arg.reg =
          Reg{(arg.bank == RegBank{0} ? AsmReg::R0 : AsmReg::V0) + next++};
    } else {
      next = 8;
      u32 size = unnamed ? util::align_up(arg.size, 8) : arg.size;
      u32 align = std::max<u32>(arg.align, unnamed ? 8 : arg.size);
      nsaa = util::align_up(nsaa, align);
      arg.stack_off = nsaa;
      nsaa += size;
    }
  }

  u32 get_stack_size() override { return nsaa; }

  void assign_ret(CCAssignment &arg) override {
    assert(!arg.byval && !arg.sret);
    u32 &next = arg.bank == RegBank{0} ? ret_ngrn : ret_nsrn;
    assert(next + arg.consecutive < 8);
    arg.reg = Reg{(arg.bank == RegBank{0} ? AsmReg::R0 : AsmReg::V0) + next++};
  }
};

/// macOS / Apple Silicon variant of `PlatformConfig`. Differs only in the
/// assembler and the calling convention (darwinpcs); same instruction
/// selection / stack discipline as Linux AArch64.
struct PlatformConfigDarwin : PlatformConfig {
  using Assembler = tpde::macho::AssemblerMachOA64;
  using DefaultCCAssigner = CCAssignerDarwinAArch64;
  // Unnamed arguments are stack-passed; va_list is an 8-byte stack cursor.
  static constexpr u32 VARARG_REG_SAVE_SIZE = 0;
};

} // namespace tpde::a64
