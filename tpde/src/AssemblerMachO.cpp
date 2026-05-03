// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/AssemblerMachO.hpp"
#include "tpde/Assembler.hpp"
#include "tpde/MachO.hpp"
#include "tpde/util/misc.hpp"

#include <cstring>

namespace tpde::macho {

namespace {

// Helper: copy a Mach-O segname/sectname (16 bytes, NUL-padded, *not* required
// to be NUL-terminated when full).
void set_seg_or_sect(char dst[16], const char *src) {
  std::memset(dst, 0, 16);
  size_t n = std::strlen(src);
  assert(n <= 16);
  std::memcpy(dst, src, n);
}

// Per-SectionKind static data: (segname, sectname, type, attrs, align,
// has_relocs, is_bss). The framework's `TargetInfo::SectionFlags` is reused;
// we pack `type | attrs` into the `flags` field and store the kind index in
// `name` so the writer can recover (segname, sectname) without a side table.
struct MachOSectionDescr {
  const char *segname;
  const char *sectname;
  u32 type_attrs;
  u8 align_log2; // We store bytes in TargetInfo::SectionFlags::align below;
                 // log2-encoding happens at write time.
  bool has_relocs;
  bool is_bss;
};

// Indexed by `unsigned(SectionKind)`; matches the order in `Assembler.hpp`.
constexpr MachOSectionDescr SECTION_DESCR[unsigned(SectionKind::Max)] = {
    /* Text       */ {"__TEXT",
                      "__text",
                      S_REGULAR | S_ATTR_PURE_INSTRUCTIONS |
                          S_ATTR_SOME_INSTRUCTIONS,
                      4,
                      true,
                      false},
    /* ReadOnly   */ {"__TEXT", "__const", S_REGULAR, 0, true, false},
    /* EHFrame    */
    {"__TEXT",
     "__eh_frame",
     S_REGULAR | S_ATTR_NO_DEAD_STRIP | S_ATTR_LIVE_SUPPORT,
     3,
     true,
     false},
    /* LSDA       */
    {"__TEXT", "__gcc_except_tab", S_REGULAR, 3, true, false},
    /* Data       */ {"__DATA", "__data", S_REGULAR, 0, true, false},
    /* DataRelRO  */
    // Mach-O has no separate `.data.rel.ro`; `__DATA,__const` plays that role.
    {"__DATA", "__const", S_REGULAR, 0, true, false},
    /* BSS        */ {"__DATA", "__bss", S_ZEROFILL, 0, false, true},
    /* ThreadData */
    {"__DATA", "__thread_data", S_THREAD_LOCAL_REGULAR, 0, true, false},
    /* ThreadBSS  */
    {"__DATA", "__thread_bss", S_THREAD_LOCAL_ZEROFILL, 0, false, true},
};

consteval auto get_macho_section_flags() {
  using SectionFlags = Assembler::TargetInfo::SectionFlags;
  std::array<SectionFlags, unsigned(SectionKind::Max)> out{};
  for (unsigned i = 0; i < unsigned(SectionKind::Max); ++i) {
    const auto &d = SECTION_DESCR[i];
    out[i] = SectionFlags{
        .type = 0,             // unused on Mach-O; type is in `flags`
        .flags = d.type_attrs, // packed S_<type> | S_ATTR_*
        .name = i,             // recover (segname, sectname) at write time
        .align = d.align_log2 ? u8(1u << d.align_log2) : u8(1),
        .has_relocs = d.has_relocs,
        .is_bss = d.is_bss,
    };
  }
  return out;
}

constexpr auto MACHO_SECTION_FLAGS = get_macho_section_flags();

} // namespace

void AssemblerMachO::reset() {
  Assembler::reset();
  local_symbols.clear();
  global_symbols.clear();
  local_sym_secs.clear();
  global_sym_secs.clear();
  strtab = StringTable();
}

SymRef AssemblerMachO::section_symbol(SecRef ref) {
  DataSection &sec = get_section(ref);
  if (sec.sym.valid()) {
    return sec.sym;
  }
  // Hand back a synthetic SymRef carrying the SecRef in its low bits and
  // SECTION_SYM_TAG in the high bits. We deliberately do NOT add an
  // nlist_64 entry: the object writer expands relocations against this ref
  // into `r_extern=0` records (section-ordinal form), and the JIT mapper
  // resolves it to `mapped_addr + section.addr`. Avoids polluting the
  // symbol table with anonymous "section anchor" entries that Apple `ld`
  // misinterprets (especially for `__eh_frame`, where any symbol triggers
  // unwind-related warnings).
  SymRef sym(ref.id() | SECTION_SYM_TAG);
  sec.sym = sym;
  return sym;
}

SymRef AssemblerMachO::sym_add(std::string_view name, SymBinding binding) {
  size_t str_off = strtab.add(name);
  nlist_64 sym{
      .n_strx = static_cast<u32>(str_off),
      .n_type = 0, // set below
      .n_sect = 0, // NO_SECT — filled in by sym_def or section sort
      .n_desc = 0,
      .n_value = 0,
  };

  switch (binding) {
    using enum SymBinding;
  case LOCAL: {
    sym.n_type = 0; // becomes N_SECT once defined
    local_symbols.push_back(sym);
    local_sym_secs.push_back(SecRef{});
    assert(local_symbols.size() < 0x8000'0000u);
    return SymRef(static_cast<u32>(local_symbols.size() - 1));
  }
  case WEAK: {
    sym.n_type = N_EXT; // becomes N_SECT|N_EXT once defined
    sym.n_desc = N_WEAK_DEF;
    global_symbols.push_back(sym);
    global_sym_secs.push_back(SecRef{});
    assert(global_symbols.size() < 0x8000'0000u);
    return SymRef(static_cast<u32>(global_symbols.size() - 1) | 0x8000'0000u);
  }
  case GLOBAL: {
    sym.n_type = N_EXT; // becomes N_SECT|N_EXT once defined
    global_symbols.push_back(sym);
    global_sym_secs.push_back(SecRef{});
    assert(global_symbols.size() < 0x8000'0000u);
    return SymRef(static_cast<u32>(global_symbols.size() - 1) | 0x8000'0000u);
  }
  default: TPDE_UNREACHABLE("invalid symbol binding");
  }
}

SymRef AssemblerMachO::sym_add_undef(std::string_view name,
                                     SymBinding binding) {
  // An undefined external is N_UNDF | N_EXT. Locals can never be undefined.
  assert(binding != SymBinding::LOCAL && "undefined local has no meaning");
  SymRef ref = sym_add(name, binding);
  nlist_64 *sym = sym_ptr(ref);
  sym->n_type = N_UNDF | N_EXT;
  sym->n_sect = 0;
  return ref;
}

SymRef AssemblerMachO::sym_predef_func(std::string_view name,
                                       SymBinding binding) {
  return sym_add(name, binding);
}

SymRef AssemblerMachO::sym_predef_data(std::string_view name,
                                       SymBinding binding) {
  return sym_add(name, binding);
}

void AssemblerMachO::sym_def(SymRef sym_ref,
                             SecRef sec_ref,
                             u64 pos,
                             u64 size) {
  (void)size; // Mach-O nlist_64 has no size field; n_value carries pos only.
  nlist_64 *sym = sym_ptr(sym_ref);
  assert((sym->n_type & N_TYPE) == N_UNDF && "cannot redefine symbol");
  sym->n_type = (sym->n_type & ~N_TYPE) | N_SECT;
  sym->n_value = pos;
  if (sym_is_local(sym_ref)) {
    local_sym_secs[sym_idx(sym_ref)] = sec_ref;
  } else {
    global_sym_secs[sym_idx(sym_ref)] = sec_ref;
  }
}

// ---------------------------------------------------------------------------
// Object-file emission
// ---------------------------------------------------------------------------

std::vector<u8> AssemblerMachO::build_object_file() {
  const auto &ti = static_cast<const TargetInfoMachO &>(target_info);

  // ----- Step 1: enumerate emittable sections and assign 1-based ordinals.
  // Mach-O `n_sect` and section-by-section iteration order are the same:
  // the order we write `section_64` records is the order of the ordinals.
  struct EmitSec {
    SecRef ref;
    u32 ordinal; // 1-based n_sect ordinal
  };
  util::SmallVector<EmitSec, 8> emit_secs;
  emit_secs.reserve(sections.size());

  // The framework's `FunctionWriter::eh_init_cie` always emits a CIE (and
  // per-function FDEs) to `SectionKind::EHFrame`, even for functions that
  // don't actually need unwind info. On Mach-O the proper home for unwind
  // info is `__LD,__compact_unwind` — `__TEXT,__eh_frame` is a fallback
  // that's tricky to encode and that Apple `ld` validates very strictly
  // (e.g., it scans FDEs and checks each function pointer against symbol
  // tables). Until the compact-unwind emitter lands (M4), skip emitting
  // `__eh_frame` from the produced `.o` so trivial leaf functions can be
  // linked without phantom unwind records. The JIT path is unaffected:
  // `MachOMapper` consumes the in-memory section list, not this object
  // file, and ignores eh_frame entries it doesn't need.
  // TODO(tpde-macos M4): emit compact-unwind entries in lieu of eh_frame,
  // and only fall back to eh_frame for prologues that exceed the compact
  // encoding's expressive range.
  SecRef ehframe_ref = default_sections[unsigned(SectionKind::EHFrame)];

  for (size_t i = 0; i < sections.size(); ++i) {
    const auto &sec = sections[i];
    if (!sec) {
      continue; // sentinel slot 0 or ELF reloc-section placeholders
    }
    if (sec->size() == 0) {
      continue;
    }
    if (ehframe_ref.valid() && SecRef(u32(i)) == ehframe_ref) {
      continue; // see comment above
    }
    emit_secs.push_back({SecRef(u32(i)), u32(emit_secs.size() + 1)});
  }

  // ----- Step 2: split the symbol table into Mach-O's three groups.
  // Mach-O requires local first, then defined-externals, then undefined-
  // externals; `LC_DYSYMTAB` indices into the table identify each group.
  struct SymRecord {
    SymRef ref;
    nlist_64 sym;
  };
  util::SmallVector<SymRecord, 16> g_local, g_extdef, g_undef;
  g_local.reserve(local_symbols.size());
  g_extdef.reserve(global_symbols.size());
  g_undef.reserve(global_symbols.size());

  // Map SecRef.id() -> 1-based ordinal for nlist_64 fixup.
  std::vector<u32> sec_to_ordinal(sections.size(), 0u);
  for (auto &es : emit_secs) {
    sec_to_ordinal[es.ref.id()] = es.ordinal;
  }

  // Note: in MH_OBJECT, sections are concatenated and `section.addr` runs
  // from 0 across them. `nlist_64.n_value` must be the *virtual* address of
  // the symbol (section.addr + offset), not the section-local offset that
  // `sym_def` stores. We adjust at serialization time after section addrs
  // are assigned (Step 4 below).
  for (u32 i = 0; i < local_symbols.size(); ++i) {
    SymRef ref(i);
    nlist_64 s = local_symbols[i];
    SecRef sec = local_sym_secs[i];
    if (sec.valid() && (s.n_type & N_TYPE) == N_SECT) {
      s.n_sect = u8(sec_to_ordinal[sec.id()]);
      assert(s.n_sect != 0 && "section dropped but symbol references it");
    }
    g_local.push_back({ref, s});
  }
  for (u32 i = 0; i < global_symbols.size(); ++i) {
    SymRef ref(i | 0x8000'0000u);
    nlist_64 s = global_symbols[i];
    SecRef sec = global_sym_secs[i];
    if ((s.n_type & N_TYPE) == N_UNDF) {
      g_undef.push_back({ref, s});
    } else {
      if (sec.valid()) {
        s.n_sect = u8(sec_to_ordinal[sec.id()]);
        assert(s.n_sect != 0);
      }
      g_extdef.push_back({ref, s});
    }
  }
  const u32 nlocal = u32(g_local.size());
  const u32 nextdef = u32(g_extdef.size());
  const u32 nundef = u32(g_undef.size());
  const u32 nsyms = nlocal + nextdef + nundef;

  // Mach-O symbol *table index* (used by relocations). This is the linear
  // position once g_local | g_extdef | g_undef are concatenated.
  std::vector<u32> sym_to_table_idx(local_symbols.size() +
                                        global_symbols.size(),
                                    0u);
  auto record_sym_idx = [&](u32 base, util::SmallVector<SymRecord, 16> &v) {
    for (u32 i = 0; i < v.size(); ++i) {
      SymRef ref = v[i].ref;
      u32 storage_idx = sym_is_local(ref)
                            ? sym_idx(ref)
                            : (sym_idx(ref) + u32(local_symbols.size()));
      sym_to_table_idx[storage_idx] = base + i;
    }
  };
  record_sym_idx(0, g_local);
  record_sym_idx(nlocal, g_extdef);
  record_sym_idx(nlocal + nextdef, g_undef);

  // ----- Step 3: lay out load commands and section payload offsets.
  // Mach-O `MH_OBJECT` uses one LC_SEGMENT_64 covering everything, plus
  // LC_BUILD_VERSION, LC_SYMTAB, LC_DYSYMTAB.
  const u32 nsects = u32(emit_secs.size());
  const u32 sizeofcmds = u32(sizeof(segment_command_64) +
                             nsects * sizeof(section_64) +
                             sizeof(build_version_command) +
                             sizeof(symtab_command) +
                             sizeof(dysymtab_command));

  std::vector<u8> out;
  // Conservative initial reservation. Refined as we walk.
  out.reserve(sizeof(mach_header_64) + sizeofcmds + 4096);
  out.resize(sizeof(mach_header_64) + sizeofcmds);

  // Section payloads: file-offset for each emitted section.
  struct SecLayout {
    u32 file_off;
    u32 reloc_off;
    u32 nreloc;
  };
  util::SmallVector<SecLayout, 8> layouts;
  layouts.resize(nsects);

  // The ARM64 Mach-O writer translates each framework-level Relocation into
  // one or two `relocation_info` records. Build them per-section so we know
  // `nreloc` upfront (needed for the section_64 record).
  util::SmallVector<util::SmallVector<relocation_info, 4>, 8> sec_relocs;
  sec_relocs.resize(nsects);

  for (u32 si = 0; si < nsects; ++si) {
    DataSection &sec = get_section(emit_secs[si].ref);
    auto &out_relocs = sec_relocs[si];
    for (const Relocation &reloc : sec.relocs) {
      // Translate to `(type, length, pcrel)` per the Mach-O ARM64 ABI.
      u32 type, length, pcrel;
      bool needs_addend_record = false;
      u32 reloc_kind = reloc.type;
      switch (reloc_kind) {
      case ARM64_RELOC_UNSIGNED:
        type = ARM64_RELOC_UNSIGNED;
        length = 3;
        pcrel = 0;
        break;
      case ARM64_RELOC_BRANCH26:
        type = ARM64_RELOC_BRANCH26;
        length = 2;
        pcrel = 1;
        needs_addend_record = (reloc.addend != 0);
        break;
      case ARM64_RELOC_PAGE21:
      case ARM64_RELOC_GOT_LOAD_PAGE21:
        type = reloc_kind;
        length = 2;
        pcrel = 1;
        needs_addend_record = (reloc.addend != 0);
        break;
      case ARM64_RELOC_PAGEOFF12:
      case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
        type = reloc_kind;
        length = 2;
        pcrel = 0;
        needs_addend_record = (reloc.addend != 0);
        break;
      case ARM64_RELOC_TPDE_PCREL32: {
        // Synthetic 32-bit PC-relative diff used for FDE func-pointers in
        // `.eh_frame`. Mach-O encodes this as SUBTRACTOR(anchor) followed
        // by UNSIGNED(target). We use section-ordinal form (`r_extern=0`)
        // for the SUBTRACTOR (and for UNSIGNED when target is a section
        // anchor) to avoid polluting the symbol table with section-symbol
        // nlist entries that Apple `ld` interprets badly in `__eh_frame`.
        //
        // Math: with SUB pointing to current section S (addr = S.addr) and
        // UNSIGNED pointing to target section T (addr = T.addr), the
        // linker computes `pre_image + T.addr - S.addr` at the relocation
        // offset O. We want the encoded value to be `(T.addr + addend) -
        // (S.addr + O)`, i.e. `target_vmaddr - here_vmaddr`. Therefore
        // pre_image = addend - O.
        const u32 self_sec_ord = emit_secs[si].ordinal;
        u32 unsigned_symnum;
        u32 unsigned_extern;
        if (is_section_sym(reloc.symbol)) {
          SecRef tgt_sec = section_sym_to_secref(reloc.symbol);
          unsigned_symnum = sec_to_ordinal[tgt_sec.id()];
          unsigned_extern = 0;
        } else {
          unsigned_symnum =
              sym_to_table_idx[sym_is_local(reloc.symbol)
                                   ? sym_idx(reloc.symbol)
                                   : (sym_idx(reloc.symbol) +
                                      u32(local_symbols.size()))];
          unsigned_extern = 1;
        }

        // Patch pre-image at the relocation site: `addend - offset`.
        // The section's data is later memcpy'd into `out`; mutating it
        // here is acceptable because build_object_file is conceptually a
        // one-shot consumption of the assembler.
        if (reloc.offset + sizeof(i32) <= sec.data.size()) {
          i32 pre = i32(reloc.addend) - i32(reloc.offset);
          std::memcpy(sec.data.data() + reloc.offset, &pre, sizeof(pre));
        } else {
          assert(false && "PCREL32 offset out of section bounds");
        }

        relocation_info sub{
            .r_address = i32(reloc.offset),
            .r_word1 = pack_reloc_word1(self_sec_ord, 0, 2,
                                        /*extern=*/0,
                                        ARM64_RELOC_SUBTRACTOR),
        };
        relocation_info uns{
            .r_address = i32(reloc.offset),
            .r_word1 = pack_reloc_word1(unsigned_symnum, 0, 2,
                                        unsigned_extern,
                                        ARM64_RELOC_UNSIGNED),
        };
        out_relocs.push_back(sub);
        out_relocs.push_back(uns);
        continue;
      }
      default:
        // Unknown framework relocation type — surface so upstream maps it.
        assert(false && "unsupported Mach-O relocation type");
        continue;
      }

      u32 sym_storage_idx = sym_is_local(reloc.symbol)
                                ? sym_idx(reloc.symbol)
                                : (sym_idx(reloc.symbol) +
                                   u32(local_symbols.size()));
      u32 sym_table_idx = sym_to_table_idx[sym_storage_idx];

      if (needs_addend_record) {
        // 24-bit addend limit; flag larger ones.
        assert((reloc.addend >= -(1 << 23)) && (reloc.addend < (1 << 23)) &&
               "ARM64_RELOC_ADDEND only supports 24-bit addends");
        relocation_info ar{
            .r_address = i32(reloc.offset),
            .r_word1 = pack_reloc_word1(u32(reloc.addend) & 0xFFFFFFu,
                                        0, length,
                                        /*extern=*/0, ARM64_RELOC_ADDEND),
        };
        out_relocs.push_back(ar);
      }

      relocation_info ri{
          .r_address = i32(reloc.offset),
          .r_word1 =
              pack_reloc_word1(sym_table_idx, pcrel, length,
                               /*extern=*/1, type),
      };
      out_relocs.push_back(ri);
    }
  }

  // ----- Step 4: write section payloads in section-order.
  // `vmaddr` is the running virtual address inside the segment; for
  // MH_OBJECT it doubles as `section_64.addr`.
  u64 segment_vmaddr = 0;
  u64 file_off_after_payloads = out.size();
  // Emit non-zerofill payloads first; ZEROFILL sections live "in segment
  // memory but not on disk" — their `offset` field must be zero.
  for (u32 si = 0; si < nsects; ++si) {
    DataSection &sec = get_section(emit_secs[si].ref);
    bool is_zf = sec.is_virtual;
    // Align on disk (and segment_vmaddr) according to section alignment.
    u32 sec_align = sec.align ? sec.align : 1;
    file_off_after_payloads = util::align_up(file_off_after_payloads,
                                             sec_align);
    segment_vmaddr = util::align_up(segment_vmaddr, sec_align);
    layouts[si].file_off = is_zf ? 0u : u32(file_off_after_payloads);
    if (!is_zf) {
      out.resize(file_off_after_payloads + sec.data.size());
      std::memcpy(out.data() + file_off_after_payloads, sec.data.data(),
                  sec.data.size());
      file_off_after_payloads += sec.data.size();
    }
    sec.addr = segment_vmaddr;
    segment_vmaddr += sec.size();
  }

  // Now that section addrs are assigned, fix up nlist_64.n_value for every
  // defined symbol: Mach-O wants the absolute vmaddr, not the section-local
  // offset that sym_def stored.
  auto fixup_n_value = [&](util::SmallVector<SymRecord, 16> &v) {
    for (auto &r : v) {
      nlist_64 &s = r.sym;
      if ((s.n_type & N_TYPE) != N_SECT || s.n_sect == 0) {
        continue;
      }
      SecRef sec_ref = AssemblerMachO::sym_is_local(r.ref)
                           ? local_sym_secs[AssemblerMachO::sym_idx(r.ref)]
                           : global_sym_secs[AssemblerMachO::sym_idx(r.ref)];
      if (sec_ref.valid()) {
        s.n_value += get_section(sec_ref).addr;
      }
    }
  };
  fixup_n_value(g_local);
  fixup_n_value(g_extdef);

  // ----- Step 5: write per-section relocation tables.
  for (u32 si = 0; si < nsects; ++si) {
    auto &out_relocs = sec_relocs[si];
    layouts[si].nreloc = u32(out_relocs.size());
    if (out_relocs.empty()) {
      layouts[si].reloc_off = 0;
      continue;
    }
    layouts[si].reloc_off = u32(out.size());
    out.insert(out.end(),
               reinterpret_cast<const u8 *>(out_relocs.data()),
               reinterpret_cast<const u8 *>(out_relocs.data() +
                                            out_relocs.size()));
  }

  // ----- Step 6: write symbol table and string table.
  // Mach-O symbol-name strings need a leading underscore for C symbols. We
  // build a fresh string table at write time so the in-memory representation
  // can stay un-prefixed (matching what `dlsym(RTLD_DEFAULT, ...)` expects).
  util::SmallVector<u8, 256> out_strtab;
  out_strtab.push_back(0); // strtab[0] must be empty

  auto map_str = [&](u32 in_n_strx) -> u32 {
    if (in_n_strx == 0) {
      return 0;
    }
    const char *name = strtab.data() + in_n_strx;
    size_t off = out_strtab.size();
    // Underscore-prefix every named symbol on Mach-O.
    out_strtab.push_back('_');
    size_t name_len = std::strlen(name);
    for (size_t i = 0; i < name_len; ++i) {
      out_strtab.push_back(u8(name[i]));
    }
    out_strtab.push_back(0);
    return u32(off);
  };

  // Rewrite n_strx for the three groups before serializing nlist_64s.
  for (auto &r : g_local) {
    r.sym.n_strx = map_str(r.sym.n_strx);
  }
  for (auto &r : g_extdef) {
    r.sym.n_strx = map_str(r.sym.n_strx);
  }
  for (auto &r : g_undef) {
    r.sym.n_strx = map_str(r.sym.n_strx);
  }

  u32 symoff = u32(out.size());
  auto append_syms = [&](util::SmallVector<SymRecord, 16> &v) {
    for (auto &r : v) {
      out.insert(out.end(), reinterpret_cast<const u8 *>(&r.sym),
                 reinterpret_cast<const u8 *>(&r.sym) + sizeof(nlist_64));
    }
  };
  append_syms(g_local);
  append_syms(g_extdef);
  append_syms(g_undef);

  u32 stroff = u32(out.size());
  out.insert(out.end(), out_strtab.begin(), out_strtab.end());
  // 8-byte align the file size for cleanliness.
  while (out.size() % 8) {
    out.push_back(0);
  }
  u32 strsize = u32(out.size() - stroff);

  // ----- Step 7: now that we know all offsets, fill in headers.
  auto *hdr = reinterpret_cast<mach_header_64 *>(out.data());
  hdr->magic = MH_MAGIC_64;
  hdr->cputype = ti.mh_cputype;
  hdr->cpusubtype = ti.mh_cpusubtype;
  hdr->filetype = MH_OBJECT;
  // Load command count: 1 (segment) + 1 (build) + 1 (symtab) + 1 (dysymtab).
  hdr->ncmds = 4;
  hdr->sizeofcmds = sizeofcmds;
  hdr->flags = MH_SUBSECTIONS_VIA_SYMBOLS;
  hdr->reserved = 0;

  u8 *lc = out.data() + sizeof(mach_header_64);

  // LC_SEGMENT_64 (with N section_64 records following)
  {
    auto *seg = reinterpret_cast<segment_command_64 *>(lc);
    seg->cmd = LC_SEGMENT_64;
    seg->cmdsize = u32(sizeof(segment_command_64) +
                       nsects * sizeof(section_64));
    set_seg_or_sect(seg->segname, "");
    seg->vmaddr = 0;
    seg->vmsize = segment_vmaddr;
    // file_off of first section's data = end of all load commands.
    // Compute `filesize` = sum of non-zerofill section sizes (with padding).
    u64 file_lo = ~u64(0), file_hi = 0;
    for (u32 si = 0; si < nsects; ++si) {
      DataSection &sec = get_section(emit_secs[si].ref);
      if (sec.is_virtual) {
        continue;
      }
      file_lo = std::min<u64>(file_lo, layouts[si].file_off);
      file_hi = std::max<u64>(file_hi, u64(layouts[si].file_off) +
                                          sec.data.size());
    }
    if (file_lo == ~u64(0)) {
      file_lo = sizeof(mach_header_64) + sizeofcmds;
      file_hi = file_lo;
    }
    seg->fileoff = file_lo;
    seg->filesize = file_hi - file_lo;
    seg->maxprot = 7; // RWX — linker decides per-section
    seg->initprot = 7;
    seg->nsects = nsects;
    seg->flags = 0;

    auto *secs = reinterpret_cast<section_64 *>(lc +
                                                sizeof(segment_command_64));
    for (u32 si = 0; si < nsects; ++si) {
      DataSection &sec = get_section(emit_secs[si].ref);
      const auto &d = SECTION_DESCR[sec.name];
      auto &s = secs[si];
      set_seg_or_sect(s.sectname, d.sectname);
      set_seg_or_sect(s.segname, d.segname);
      s.addr = sec.addr;
      s.size = sec.size();
      s.offset = layouts[si].file_off;
      // Mach-O wants log2(alignment); clamp to [0, 15].
      u32 a = sec.align ? sec.align : 1;
      u32 align_log2 = 0;
      while ((1u << align_log2) < a && align_log2 < 15) {
        ++align_log2;
      }
      s.align = align_log2;
      s.reloff = layouts[si].reloc_off;
      s.nreloc = layouts[si].nreloc;
      s.flags = d.type_attrs;
      s.reserved1 = 0;
      s.reserved2 = 0;
      s.reserved3 = 0;
    }
    lc += seg->cmdsize;
  }

  // LC_BUILD_VERSION
  {
    auto *bv = reinterpret_cast<build_version_command *>(lc);
    bv->cmd = LC_BUILD_VERSION;
    bv->cmdsize = sizeof(build_version_command);
    bv->platform = ti.build_platform;
    bv->minos = ti.build_minos;
    bv->sdk = ti.build_minos;
    bv->ntools = 0;
    lc += bv->cmdsize;
  }

  // LC_SYMTAB
  {
    auto *st = reinterpret_cast<symtab_command *>(lc);
    st->cmd = LC_SYMTAB;
    st->cmdsize = sizeof(symtab_command);
    st->symoff = symoff;
    st->nsyms = nsyms;
    st->stroff = stroff;
    st->strsize = strsize;
    lc += st->cmdsize;
  }

  // LC_DYSYMTAB
  {
    auto *ds = reinterpret_cast<dysymtab_command *>(lc);
    std::memset(ds, 0, sizeof(*ds));
    ds->cmd = LC_DYSYMTAB;
    ds->cmdsize = sizeof(dysymtab_command);
    ds->ilocalsym = 0;
    ds->nlocalsym = nlocal;
    ds->iextdefsym = nlocal;
    ds->nextdefsym = nextdef;
    ds->iundefsym = nlocal + nextdef;
    ds->nundefsym = nundef;
    lc += ds->cmdsize;
  }

  return out;
}

// ---------------------------------------------------------------------------
// Per-target subclass (AArch64 / arm64-apple-macosx)
// ---------------------------------------------------------------------------

// clang-format off
const AssemblerMachO::TargetInfoMachO AssemblerMachOA64::TARGET_INFO{
  {
    .reloc_pc32 = ARM64_RELOC_TPDE_PCREL32,
    .reloc_abs64 = ARM64_RELOC_UNSIGNED,
    .section_flags = MACHO_SECTION_FLAGS,
  },
  CPU_TYPE_ARM64,
  CPU_SUBTYPE_ARM64_ALL,
  PLATFORM_MACOS,
  (11u << 16), // macOS 11.0 — first Apple Silicon release
};
// clang-format on

} // namespace tpde::macho
