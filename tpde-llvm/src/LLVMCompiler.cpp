// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde-llvm/LLVMCompiler.hpp"

#include <llvm/TargetParser/Triple.h>
#include <memory>

#ifdef TPDE_ARCH_AARCH64
  #include "arm64/LLVMCompilerArm64.hpp"
  #ifdef TPDE_LLVM_ARM64_DARWIN
    #include "arm64/LLVMCompilerArm64Darwin.hpp"
  #endif
#endif
#ifdef TPDE_ARCH_X86_64
  #include "x64/LLVMCompilerX64.hpp"
#endif

namespace tpde_llvm {

LLVMCompiler::~LLVMCompiler() = default;

std::unique_ptr<LLVMCompiler> LLVMCompiler::create(const llvm::Triple &triple) {
  switch (triple.getArch()) {
#ifdef TPDE_ARCH_X86_64
  case llvm::Triple::x86_64: return x64::create_compiler(triple);
#endif
#ifdef TPDE_ARCH_AARCH64
  case llvm::Triple::aarch64:
  #ifdef TPDE_LLVM_ARM64_DARWIN
    if (triple.isOSDarwin()) {
      return arm64::create_compiler_darwin(triple);
    }
  #endif
    return arm64::create_compiler(triple);
#endif
  default: return nullptr;
  }
}

} // namespace tpde_llvm
