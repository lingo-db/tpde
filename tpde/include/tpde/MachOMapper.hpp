// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <string_view>
#include <utility>

#include "base.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/util/SmallVector.hpp"
#include "tpde/util/function_ref.hpp"

namespace tpde::macho {

/// In-process arm64 Mach-O linker. Executable pages use MAP_JIT; data
/// remains writable in a separate allocation. Unwind information uses DWARF.
class MachOMapper {
public:
  using SymbolResolver = util::function_ref<void *(std::string_view)>;

private:
  u8 *mapped_addr = nullptr;
  size_t mapped_size = 0;
  u8 *data_addr = nullptr;
  size_t data_size = 0;

  util::SmallVector<void *, 64> sym_addrs;

  util::SmallVector<void *, 16> registered_fdes;
  util::SmallVector<void (*)(), 4> destructors;

public:
  MachOMapper() = default;
  ~MachOMapper() { reset(); }

  MachOMapper(const MachOMapper &) = delete;
  MachOMapper(MachOMapper &&) = delete;
  MachOMapper &operator=(const MachOMapper &) = delete;
  MachOMapper &operator=(MachOMapper &&) = delete;

  /// Run terminators, deregister unwind records, and release both allocations.
  void reset();

  /// Map and link the contents of `assembler` into a fresh JIT region.
  /// The resolver receives the assembler's symbol names unchanged and is the
  /// only source of external addresses. Null rejects a strong reference and
  /// leaves a weak reference unresolved. Initializers run after linking.
  /// Returns false on linking failure and leaves the mapper empty.
  bool map(AssemblerMachO &assembler, SymbolResolver resolver);

  /// Resolved address of a symbol, or nullptr if undefined-and-unresolved.
  void *get_sym_addr(SymRef sym) const;

  /// Executable allocation only; data and unwind records are separate.
  std::pair<void *, size_t> get_mapped_range() const;
};

} // namespace tpde::macho
