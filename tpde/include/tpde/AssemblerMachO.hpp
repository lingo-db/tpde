// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cassert>
#include <span>
#include <string_view>
#include <vector>

#include "base.hpp"
#include "tpde/Assembler.hpp"
#include "tpde/MachO.hpp"
#include "tpde/StringTable.hpp"
#include "util/SmallVector.hpp"

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
  /// nlist_64 entries split into the three Mach-O ordering buckets.
  /// Local first, then defined-externals, then undefined-externals — the
  /// `LC_DYSYMTAB` ranges encode where each group starts. We assign
  /// `SymRef.id()` so that `sym_is_local()` / `sym_idx()` mirror the same
  /// scheme `AssemblerElf` uses (high bit = non-local, low bits = index into
  /// either the local or the global vector).
  std::vector<nlist_64> local_symbols;
  std::vector<nlist_64> global_symbols;

  /// Parallel to `*_symbols`: for each symbol, the SecRef of the section it
  /// is defined in, or an invalid SecRef for undefined references.
  /// Mach-O encodes this in `nlist_64.n_sect` (1-based section ordinal),
  /// but we cannot fill that in until section ordering is finalized.
  std::vector<SecRef> local_sym_secs;
  std::vector<SecRef> global_sym_secs;

  /// String table. Mach-O strtab also begins with a zero byte.
  StringTable strtab;

public:
  explicit AssemblerMachO(const TargetInfoMachO &target_info)
      : Assembler(target_info) {
    // Reserve sections[0] as a sentinel so the first real section has
    // SecRef::id() == 1. This matters because `SecRef::valid()` is defined
    // as `val != 0` — without the placeholder, the first user-requested
    // section would round-trip through `default_sections[]` with id==0 and
    // be reported as invalid. AssemblerElf gets this for free via its
    // larger `init_sections` reservation; we need an explicit no-op slot.
    sections.emplace_back(nullptr);
  }

  void reset() override;

  /// Section symbol is currently always synthesized lazily; Mach-O doesn't
  /// require section-named symbols, but the framework's EH-frame writer asks
  /// for one to anchor `reloc_pc32` records.
  SymRef section_symbol(SecRef) override;

  /// Section names in Mach-O live on the section_64 record itself, not in a
  /// separate string table — so renaming requires storing the name. Not
  /// supported for the minimal back-end yet.
  void rename_section(SecRef, std::string_view) override {
    assert(false && "rename_section unsupported on Mach-O back-end");
  }

  // ---- Symbols ----

  [[nodiscard]] SymRef sym_add_undef(std::string_view name,
                                     SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_func(std::string_view name,
                                       SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_data(std::string_view name,
                                       SymBinding binding) override;
  [[nodiscard]] SymRef sym_predef_tls(std::string_view, SymBinding) override {
    assert(false && "TLS unsupported on Mach-O back-end");
    return SymRef();
  }

  void sym_def(SymRef sym, SecRef sec, u64 pos, u64 size) override;

  /// Returns true if the SymRef encodes a local symbol; mirrors
  /// `AssemblerElf::sym_is_local`. We use the same high-bit scheme so the
  /// JIT mapper can index symbol arrays uniformly.
  [[nodiscard]] static bool sym_is_local(SymRef sym) {
    return (sym.id() & 0x8000'0000u) == 0;
  }
  [[nodiscard]] static u32 sym_idx(SymRef sym) {
    return sym.id() & ~0x8000'0000u;
  }

  /// Resolve a SymRef to its `nlist_64` slot. Public so MachOMapper can use
  /// it. Returned pointer is only valid until the next `sym_add*` call.
  [[nodiscard]] nlist_64 *sym_ptr(SymRef sym) {
    return sym_is_local(sym) ? &local_symbols[sym_idx(sym)]
                             : &global_symbols[sym_idx(sym)];
  }
  [[nodiscard]] const nlist_64 *sym_ptr(SymRef sym) const {
    return sym_is_local(sym) ? &local_symbols[sym_idx(sym)]
                             : &global_symbols[sym_idx(sym)];
  }

  /// SecRef of the section in which sym is defined, or an invalid SecRef for
  /// undefined externals.
  [[nodiscard]] SecRef sym_section(SymRef sym) const {
    return sym_is_local(sym) ? local_sym_secs[sym_idx(sym)]
                             : global_sym_secs[sym_idx(sym)];
  }

  /// Return the un-prefixed symbol name (no leading underscore). The
  /// underscore is added at object-file write time.
  [[nodiscard]] const char *sym_name(SymRef sym) const {
    if (is_section_sym(sym)) {
      return "";
    }
    return strtab.data() + sym_ptr(sym)->n_strx;
  }

  // ---- Section-symbol encoding ----
  //
  // Mach-O relocations can reference a section by 1-based ordinal (the
  // `r_extern=0` form) instead of going through a symbol-table entry. The
  // framework's EH writer asks for `section_symbol(SecRef)` to anchor
  // PCREL32 relocations; rather than emitting an anonymous nlist_64 entry
  // (which Apple `ld` interprets as a "real" symbol living inside that
  // section, and warns about for `__eh_frame`), we hand back a synthetic
  // SymRef tagged with a high bit. The object writer translates relocations
  // whose symbol is tagged into the `r_extern=0` form, and the JIT mapper
  // resolves them to `mapped_addr + section.addr`.

  /// The tag bit on a SymRef value indicating "section symbol", encoded as
  /// `tag | sec_ref_id`. Chosen above `0x8000'0000` so it doesn't collide
  /// with the local/global bit used for normal symbols.
  static constexpr u32 SECTION_SYM_TAG = 0x4000'0000u;

  [[nodiscard]] static bool is_section_sym(SymRef sym) {
    return (sym.id() & SECTION_SYM_TAG) != 0;
  }
  [[nodiscard]] static SecRef section_sym_to_secref(SymRef sym) {
    assert(is_section_sym(sym));
    return SecRef(sym.id() & ~SECTION_SYM_TAG);
  }

  // ---- Output ----

  std::vector<u8> build_object_file() override;

private:
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
  // TLS isn't supported on Mach-O yet (see rough plan §7); the TLS-related
  // entries are sentinels — the Mach-O writer asserts if it encounters
  // them, and `tpde-llvm` should reject `thread_local` globals before
  // codegen reaches this point.
  static constexpr u32 RELOC_CALL = ARM64_RELOC_BRANCH26;
  static constexpr u32 RELOC_PAGE21 = ARM64_RELOC_PAGE21;
  static constexpr u32 RELOC_PAGEOFF12_LDST128 = ARM64_RELOC_PAGEOFF12;
  static constexpr u32 RELOC_TLSDESC_PAGE21 = ~u32(0);
  static constexpr u32 RELOC_TLSDESC_LD64_LO12 = ~u32(0);
  static constexpr u32 RELOC_TLSDESC_ADD_LO12 = ~u32(0);
  static constexpr u32 RELOC_TLSDESC_CALL = ~u32(0);

  AssemblerMachOA64() : AssemblerMachO(TARGET_INFO) {}
};

} // namespace tpde::macho
