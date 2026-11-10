// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cassert>
#include <string_view>
#include <vector>

#include "base.hpp"
#include "tpde/Assembler.hpp"
#include "tpde/MachO.hpp"
#include "tpde/StringTable.hpp"

namespace tpde::macho {

/// Mach-O `MH_OBJECT` assembler. Mirrors `AssemblerElf` in shape but writes
/// the Mach-O wire format. Only the AArch64 / `arm64-apple-macosx` target is
/// supported today (see `AssemblerMachOA64`).
///
/// The intent is that `MachOMapper` (the in-process JIT linker) and
/// `build_object_file()` consume the same in-memory state — the Mapper does
/// not parse the produced object blob.
class AssemblerMachO : public Assembler {
  friend class MachOMapper;

protected:
  struct TargetInfoMachO : Assembler::TargetInfo {
    /// Mach-O `cputype` field for the produced object header.
    i32 mh_cputype;
    /// Mach-O `cpusubtype` field for the produced object header.
    i32 mh_cpusubtype;
    /// `LC_BUILD_VERSION.platform` value (e.g. PLATFORM_MACOS).
    u32 build_platform;
    /// `LC_BUILD_VERSION.minos` packed (major<<16)|(minor<<8)|patch.
    u32 build_minos;
  };

private:
  struct Symbol {
    nlist_64 entry;
    SecRef section;
  };
  // SymRef indexes this vector directly; slot zero is invalid. On-disk
  // local/defined/undefined ordering is established only during serialization.
  std::vector<Symbol> symbols;

  /// String table. Mach-O strtab also begins with a zero byte.
  StringTable strtab;

public:
  enum class SymVisibility : u8 {
    DEFAULT,
    INTERNAL,
    HIDDEN,
    PROTECTED
  };

  explicit AssemblerMachO(const TargetInfoMachO &target_info)
      : Assembler(target_info) {
    reset();
  }

  void reset() override;

  /// Reuse a symbol at the section start, or create a local relocation anchor.
  SymRef section_symbol(SecRef) override;

  /// Section names in Mach-O live on the section_64 record itself, not in a
  /// separate string table — so renaming requires storing the name. Not
  /// supported for the minimal back-end yet.
  void rename_section(SecRef, std::string_view) override {
    TPDE_FATAL("custom sections unsupported on Mach-O");
  }

  // ---- Symbols ----

  [[nodiscard]] SymRef sym_add_undef(std::string_view name,
                                     SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_func(std::string_view name,
                                       SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_data(std::string_view name,
                                       SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_tls(std::string_view, SymBinding) override {
    TPDE_FATAL("TLS unsupported on Mach-O");
  }

  void sym_def(SymRef sym, SecRef sec, u64 pos, u64 size) override;

  /// Copy section/value from `src` onto `dst` (Mach-O `nlist_64`'s
  /// n_sect / n_value). Used for aliases.
  void sym_copy(SymRef dst, SymRef src);

  /// Mach-O has no full STV_* visibility model. The closest analogue
  /// is `N_PEXT` (private external) which makes the symbol effectively
  /// static after final linking — that's what HIDDEN means for users.
  /// Other visibility values (INTERNAL, PROTECTED) have no Mach-O
  /// counterpart and are no-ops; DEFAULT means "external" already.
  void sym_set_visibility(SymRef sym, SymVisibility visibility);

  /// Mach-O equivalent of ELF `SHF_GNU_RETAIN`: set the
  /// `S_ATTR_NO_DEAD_STRIP` attribute on the section so `ld` keeps it
  /// even when no symbol points at it.
  void set_section_retain(SecRef sec);

  /// Create a Mach-O static-init or static-term section. ELF uses
  /// `.init_array` / `.fini_array`; Mach-O uses
  /// `__DATA,__mod_init_func` / `__DATA,__mod_term_func`. The
  /// `group` parameter is ignored — Mach-O has no section groups.
  [[nodiscard]] SecRef create_structor_section(bool init,
                                               SecRef group = SecRef());

  [[nodiscard]] SecRef create_group_section(SymRef, bool) {
    TPDE_FATAL("COMDAT groups unsupported on Mach-O");
  }

  void add_to_group(SecRef, SecRef) {
    TPDE_FATAL("COMDAT groups unsupported on Mach-O");
  }

  [[nodiscard]] nlist_64 *sym_ptr(SymRef sym) {
    return &symbols[sym.id()].entry;
  }
  [[nodiscard]] const nlist_64 *sym_ptr(SymRef sym) const {
    return &symbols[sym.id()].entry;
  }
  [[nodiscard]] SecRef sym_section(SymRef sym) const {
    return symbols[sym.id()].section;
  }

  /// Return the un-prefixed symbol name (no leading underscore). The
  /// underscore is added at object-file write time.
  [[nodiscard]] const char *sym_name(SymRef sym) const {
    return strtab.data() + sym_ptr(sym)->n_strx;
  }

  // ---- Output ----

  std::vector<u8> build_object_file() override;

private:
  SymRef find_symbol_at(SecRef, u64 offset) const;
  [[nodiscard]] SymRef sym_add(std::string_view name, SymBinding binding);
};

/// AArch64 / `arm64-apple-macosx` concrete subclass. Picks the cputype, the
/// per-section flag table, and the deployment target.
class AssemblerMachOA64 final : public AssemblerMachO {
  static const TargetInfoMachO TARGET_INFO;

public:
  // Mirror of `AssemblerElfA64`'s per-target reloc kinds. The compiler
  // selects between them through `Config::Assembler::RELOC_*` so the same
  // codegen template is back-end agnostic.
  // TLS relocations are unsupported; the LLVM frontend rejects TLS globals.
  static constexpr u32 RELOC_CALL = ARM64_RELOC_BRANCH26;
  static constexpr u32 RELOC_PAGE21 = ARM64_RELOC_PAGE21;
  // Mach-O's PAGEOFF12 is a single relocation kind regardless of the
  // patched opcode (linker reads the instruction at r_address and
  // infers the implicit shift). Both the ADD-immediate and the LDR-Q
  // (128-bit) flavors map onto it.
  static constexpr u32 RELOC_PAGEOFF12_ADD = ARM64_RELOC_PAGEOFF12;
  static constexpr u32 RELOC_PAGEOFF12_LDST128 = ARM64_RELOC_PAGEOFF12;
  static constexpr u32 RELOC_GOT_PAGE21 = ARM64_RELOC_GOT_LOAD_PAGE21;
  static constexpr u32 RELOC_GOT_PAGEOFF12 = ARM64_RELOC_GOT_LOAD_PAGEOFF12;

  AssemblerMachOA64() : AssemblerMachO(TARGET_INFO) {}
};

} // namespace tpde::macho
