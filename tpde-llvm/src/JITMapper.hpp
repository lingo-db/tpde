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

/// Dispatch to the mapper selected by the assembler's object format.
class JITMapperImpl {
  using GlobalMap = llvm::DenseMap<const llvm::GlobalValue *, tpde::SymRef>;

  std::variant<tpde::elf::ElfMapper, tpde::macho::MachOMapper> mapper;

  GlobalMap globals;

public:
  JITMapperImpl(GlobalMap &&globals) : globals(std::move(globals)) {}

  bool map(tpde::elf::AssemblerElf &, tpde::elf::ElfMapper::SymbolResolver);

  /// Mach-O counterpart of `map(AssemblerElf&, ...)`.
  bool map(tpde::macho::AssemblerMachO &,
           tpde::macho::MachOMapper::SymbolResolver);

  void *lookup_global(llvm::GlobalValue *gv) const {
    tpde::SymRef sym = globals.lookup(gv);
    return std::visit([sym](const auto &m) { return m.get_sym_addr(sym); },
                      mapper);
  }

  std::pair<void *, size_t> get_mapped_range() const {
    return std::visit([](const auto &m) { return m.get_mapped_range(); },
                      mapper);
  }
};

} // namespace tpde_llvm
