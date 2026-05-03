// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <memory>

#include "tpde-llvm/LLVMCompiler.hpp"

namespace llvm {
class Triple;
} // namespace llvm

namespace tpde_llvm::arm64 {

/// Darwin sibling of `arm64::create_compiler` — returns an
/// LLVMCompilerArm64Darwin (Mach-O assembler + darwinpcs CC). The
/// public LLVMCompiler::create dispatches on `triple.isOSDarwin()`.
/// Currently a duplicated class rather than a templated one (see
/// rough plan §4.8 Option F+duplicate); the two classes share
/// LLVMCompilerBase but otherwise re-implement everything per arch.
std::unique_ptr<LLVMCompiler> create_compiler_darwin(const llvm::Triple &);

} // namespace tpde_llvm::arm64
