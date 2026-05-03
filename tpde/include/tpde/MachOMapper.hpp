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

/// In-process JIT linker for the Mach-O back-end. The Apple Silicon counterpart
/// to `tpde::elf::ElfMapper`: walks the assembler's section list, allocates a
/// `MAP_JIT` region, copies section bytes in (under a `pthread_jit_write_protect_np`
/// window), applies relocations, invalidates the I-cache, and exposes resolved
/// symbol addresses.
///
/// The minimal back-end (M0/M1) supports leaf functions with no relocations and
/// no exception handling — sufficient to JIT something like `int add(int,int)`
/// and call it from C++. Compact-unwind / EH dynamic registration is a later
/// milestone.
class MachOMapper {
public:
  using SymbolResolver = util::function_ref<void *(std::string_view)>;

private:
  u8 *mapped_addr = nullptr;
  size_t mapped_size = 0;

  // sym_addrs[i] — local syms first, then globals, mirroring the
  // AssemblerMachO storage order.
  u32 local_sym_count = 0;
  util::SmallVector<void *, 64> sym_addrs;

  // For libunwind dynamic registration: pointers + sizes of the
  // post-relocation `__compact_unwind` and `__eh_frame` payloads
  // inside the JIT region. Either may be null if the assembler
  // produced no data of that kind. Used by `reset()` to deregister
  // and by the global `find_unwind_*` callback (see MachOMapper.cpp).
  u8 *cu_section_addr = nullptr;
  size_t cu_section_size = 0;
  u8 *eh_frame_addr = nullptr;
  size_t eh_frame_size = 0;

public:
  MachOMapper() = default;
  ~MachOMapper() { reset(); }

  MachOMapper(const MachOMapper &) = delete;
  MachOMapper(MachOMapper &&) = delete;
  MachOMapper &operator=(const MachOMapper &) = delete;
  MachOMapper &operator=(MachOMapper &&) = delete;

  void reset();

  /// Map and link the contents of `assembler` into a fresh JIT region.
  /// `resolver` is consulted for any undefined external symbols. Returns
  /// false on failure (and leaves the mapper empty).
  bool map(AssemblerMachO &assembler, SymbolResolver resolver);

  /// Resolved address of a symbol, or nullptr if undefined-and-unresolved.
  void *get_sym_addr(SymRef sym) const;

  std::pair<void *, size_t> get_mapped_range() const;
};

} // namespace tpde::macho
