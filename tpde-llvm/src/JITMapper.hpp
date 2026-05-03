// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include "tpde/AssemblerElf.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/ElfMapper.hpp"
#include "tpde/MachOMapper.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/GlobalValue.h>

#include <variant>

namespace tpde_llvm {

/// Wraps the format-specific in-process JIT linker. We keep both the
/// ELF and Mach-O mappers behind a single concrete class (rather than a
/// type-erased base + derived classes) so the public `JITMapper` opaque
/// pointer in `LLVMCompiler.hpp` doesn't have to change. The active
/// mapper is chosen by the overload of `map()` that the caller invokes,
/// driven at template-instantiation time by `Config::Assembler` in
/// `LLVMCompilerBase::compile_and_map`.
class JITMapperImpl {
  using GlobalMap = llvm::DenseMap<const llvm::GlobalValue *, tpde::SymRef>;

  // monostate when not yet mapped. After a successful `map(...)`, holds
  // exactly one of the two mapper kinds.
  std::variant<std::monostate,
               tpde::elf::ElfMapper,
               tpde::macho::MachOMapper>
      mapper;

  GlobalMap globals;

public:
  JITMapperImpl(GlobalMap &&globals) : globals(std::move(globals)) {}

  /// Map an ELF assembler's output into executable memory. The first
  /// successful `map(...)` call binds this instance to ELF; mixing
  /// kinds on the same JITMapperImpl is a programmer error.
  bool map(tpde::elf::AssemblerElf &,
           tpde::elf::ElfMapper::SymbolResolver);

  /// Mach-O counterpart of `map(AssemblerElf&, ...)`.
  bool map(tpde::macho::AssemblerMachO &,
           tpde::macho::MachOMapper::SymbolResolver);

  void *lookup_global(llvm::GlobalValue *gv) const {
    tpde::SymRef sym = globals.lookup(gv);
    return std::visit(
        [sym](const auto &m) -> void * {
          if constexpr (std::is_same_v<std::decay_t<decltype(m)>,
                                       std::monostate>) {
            return nullptr;
          } else {
            return m.get_sym_addr(sym);
          }
        },
        mapper);
  }

  std::pair<void *, size_t> get_mapped_range() const {
    return std::visit(
        [](const auto &m) -> std::pair<void *, size_t> {
          if constexpr (std::is_same_v<std::decay_t<decltype(m)>,
                                       std::monostate>) {
            return {nullptr, 0ull};
          } else {
            return m.get_mapped_range();
          }
        },
        mapper);
  }
};

} // namespace tpde_llvm
