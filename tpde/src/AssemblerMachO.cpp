// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/AssemblerMachO.hpp"
#include "tpde/Assembler.hpp"
#include "tpde/MachO.hpp"
#include "tpde/util/misc.hpp"

#include <bit>
#include <cstring>
#include <map>
#include <string>

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
  u8 align;
  bool has_relocs;
  bool is_bss;
};

// Default sections are indexed by SectionKind; structor sections follow them.
constexpr u32 INIT_SECTION = unsigned(SectionKind::Max);
constexpr u32 TERM_SECTION = INIT_SECTION + 1;
// clang-format off
constexpr MachOSectionDescr SECTION_DESCR[] = {
    {"__TEXT", "__text", S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, 16, true, false},
    {"__TEXT", "__const", S_REGULAR, 1, true, false},
    {"__TEXT", "__eh_frame", S_COALESCED | S_ATTR_LIVE_SUPPORT | S_ATTR_NO_TOC | S_ATTR_STRIP_STATIC_SYMS, 8, true, false},
    {"__TEXT", "__gcc_except_tab", S_REGULAR, 8, true, false},
    {"__DATA", "__data", S_REGULAR, 1, true, false},
    {"__DATA", "__const", S_REGULAR, 1, true, false},
    {"__DATA", "__bss", S_ZEROFILL, 1, false, true},
    {"__DATA", "__thread_data", S_THREAD_LOCAL_REGULAR, 1, true, false},
    {"__DATA", "__thread_bss", S_THREAD_LOCAL_ZEROFILL, 1, false, true},
    {"__DATA", "__mod_init_func", S_MOD_INIT_FUNC_POINTERS, 8, true, false},
    {"__DATA", "__mod_term_func", S_MOD_TERM_FUNC_POINTERS, 8, true, false},
};
// clang-format on

consteval auto get_macho_section_flags() {
  using SectionFlags = Assembler::TargetInfo::SectionFlags;
  std::array<SectionFlags, unsigned(SectionKind::Max)> out{};
  for (unsigned i = 0; i < unsigned(SectionKind::Max); ++i) {
    const auto &d = SECTION_DESCR[i];
    out[i] = SectionFlags{
        .type = 0,             // unused on Mach-O; type is in `flags`
        .flags = d.type_attrs, // packed S_<type> | S_ATTR_*
        .name = i,             // recover (segname, sectname) at write time
        .align = d.align,
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
  symbols.clear();
  symbols.emplace_back();
  strtab = StringTable();
  sections.emplace_back(nullptr);
}

SymRef AssemblerMachO::find_symbol_at(SecRef sec, u64 offset) const {
  SymRef local;
  for (u32 i = 1; i < symbols.size(); ++i) {
    const auto &sym = symbols[i];
    if (sym.section != sec || sym.entry.n_value != offset) {
      continue;
    }
    if (sym.entry.n_type & N_EXT) {
      return SymRef(i);
    }
    if (!local.valid()) {
      local = SymRef(i);
    }
  }
  return local;
}

SymRef AssemblerMachO::section_symbol(SecRef ref) {
  auto &sec = get_section(ref);
  if (!sec.sym.valid()) {
    sec.sym = find_symbol_at(ref, 0);
    if (!sec.sym.valid()) {
      sec.sym = sym_add(std::string("\1ltmp") + std::to_string(ref.id()),
                        SymBinding::LOCAL);
      sym_def(sec.sym, ref, 0, 0);
    }
  }
  return sec.sym;
}

SymRef AssemblerMachO::sym_add(std::string_view name, SymBinding binding) {
  nlist_64 entry{};
  entry.n_strx = u32(strtab.add(name));
  entry.n_type = binding == SymBinding::LOCAL ? 0 : N_EXT;
  entry.n_desc = binding == SymBinding::WEAK ? N_WEAK_DEF : 0;
  symbols.push_back({entry, SecRef{}});
  return SymRef(u32(symbols.size() - 1));
}

SymRef AssemblerMachO::sym_add_undef(std::string_view name,
                                     SymBinding binding) {
  // An undefined external is N_UNDF | N_EXT. Locals can never be undefined.
  assert(binding != SymBinding::LOCAL && "undefined local has no meaning");
  SymRef ref = sym_add(name, binding);
  nlist_64 *sym = sym_ptr(ref);
  sym->n_type = N_UNDF | N_EXT;
  sym->n_sect = 0;
  if (binding == SymBinding::WEAK) {
    sym->n_desc = N_WEAK_REF;
  }
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

void AssemblerMachO::sym_copy(SymRef dst, SymRef src) {
  // Preserve the alias's binding and visibility.
  auto &to = symbols[dst.id()];
  const auto &from = symbols[src.id()];
  to.entry.n_type = (to.entry.n_type & ~N_TYPE) | (from.entry.n_type & N_TYPE);
  to.entry.n_value = from.entry.n_value;
  to.section = from.section;
}

void AssemblerMachO::set_section_retain(SecRef sec) {
  // Mach-O encodes "do not dead-strip" as `S_ATTR_NO_DEAD_STRIP` in the
  // high 24 bits of `section.flags` (alongside the section type in the
  // low 8 bits). The framework's `flags` field already stores
  // `type | attrs` packed, so OR-in is correct.
  get_section(sec).flags |= S_ATTR_NO_DEAD_STRIP;
}

SecRef AssemblerMachO::create_structor_section(bool init, SecRef) {
  return create_section(TargetInfo::SectionFlags{
      .flags = init ? S_MOD_INIT_FUNC_POINTERS : S_MOD_TERM_FUNC_POINTERS,
      .name = init ? INIT_SECTION : TERM_SECTION,
      .align = 8,
      .has_relocs = true,
      .is_bss = false});
}

void AssemblerMachO::sym_set_visibility(SymRef sym, SymVisibility visibility) {
  nlist_64 *ns = sym_ptr(sym);
  // Clear the bit first so a later DEFAULT call can revoke an earlier
  // HIDDEN. Other visibility kinds have no Mach-O analogue.
  ns->n_type &= ~N_PEXT;
  if (visibility == SymVisibility::HIDDEN) {
    ns->n_type |= N_PEXT;
  }
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
  symbols[sym_ref.id()].section = sec_ref;
}

// ---------------------------------------------------------------------------
// Object-file emission
// ---------------------------------------------------------------------------

std::vector<u8> AssemblerMachO::build_object_file() {
  const auto &ti = static_cast<const TargetInfoMachO &>(target_info);
  util::SmallVector<SecRef, 8> emit_secs;
  std::vector<u32> sec_to_ordinal(sections.size());
  u32 segment_align = 1;
  for (const auto &sec : sections) {
    if (!sec) {
      continue;
    }
    if (!std::has_single_bit(sec->align)) {
      TPDE_FATAL("Mach-O section alignment must be a power of two");
    }
    if (!sec->is_virtual) {
      segment_align = std::max(segment_align, sec->align);
    }
    emit_secs.push_back(sec->get_ref());
    sec_to_ordinal[sec->get_ref().id()] = u32(emit_secs.size());
    // SUBTRACTOR relocations require a real source symbol before we order
    // the symbol table. Keep zero-sized sections with symbol definitions.
    for (const auto &reloc : sec->relocs) {
      if (reloc.type == ARM64_RELOC_TPDE_PCREL32) {
        (void)section_symbol(sec->get_ref());
        break;
      }
    }
  }
  // Index function atoms once: searching the full symbol table for each FDE
  // makes serialization quadratic in the number of functions.
  std::map<std::pair<u32, u64>, SymRef> symbols_at;
  for (u32 i = 1; i < symbols.size(); ++i) {
    const auto &sym = symbols[i];
    if (!sym.section.valid()) {
      continue;
    }
    auto [it, inserted] = symbols_at.try_emplace(
        std::pair{sym.section.id(), sym.entry.n_value}, SymRef(i));
    if (!inserted && !(sym_ptr(it->second)->n_type & N_EXT) &&
        (sym.entry.n_type & N_EXT)) {
      it->second = SymRef(i);
    }
  }

  const u32 nsects = u32(emit_secs.size());
  if (nsects > 255) {
    TPDE_FATAL("too many Mach-O sections");
  }

  // File order differs from stable in-memory SymRefs. Partition references
  // into locals, defined externals, and undefined externals without copying
  // or mutating the symbols themselves.
  std::vector<SymRef> sym_order;
  std::vector<u32> sym_to_table_idx(symbols.size());
  std::array<u32, 4> group_offsets{};
  for (u32 group = 0; group < 3; ++group) {
    for (u32 i = 1; i < symbols.size(); ++i) {
      const auto &sym = symbols[i].entry;
      u32 kind = !(sym.n_type & N_EXT)             ? 0
                 : (sym.n_type & N_TYPE) == N_UNDF ? 2
                                                   : 1;
      if (kind != group) {
        continue;
      }
      sym_to_table_idx[i] = u32(sym_order.size());
      sym_order.push_back(SymRef(i));
    }
    group_offsets[group + 1] = u32(sym_order.size());
  }
  const u32 nlocal = group_offsets[1];
  const u32 nextdef = group_offsets[2] - group_offsets[1];
  const u32 nundef = group_offsets[3] - group_offsets[2];
  const u32 nsyms = u32(sym_order.size());
  const u32 sizeofcmds =
      u32(sizeof(segment_command_64) + nsects * sizeof(section_64) +
          sizeof(build_version_command) + sizeof(symtab_command) +
          sizeof(dysymtab_command));
  // Start the file payload at the same alignment phase as virtual address
  // zero. Otherwise file padding can exceed the segment's virtual extent.
  std::vector<u8> out(
      util::align_up(sizeof(mach_header_64) + sizeofcmds, segment_align));
  struct SecLayout {
    u64 addr = 0;
    u32 file_off = 0;
    u32 reloc_off = 0;
    u32 nreloc = 0;
  };
  std::vector<SecLayout> layouts(nsects);
  u64 segment_vmaddr = 0;
  for (u32 si = 0; si < nsects; ++si) {
    const auto &sec = get_section(emit_secs[si]);
    auto &layout = layouts[si];
    layout.addr = util::align_up(segment_vmaddr, sec.align);
    segment_vmaddr = layout.addr + sec.size();
    if (!sec.is_virtual) {
      layout.file_off = u32(util::align_up(out.size(), sec.align));
      out.resize(layout.file_off + sec.data.size());
      std::memcpy(
          out.data() + layout.file_off, sec.data.data(), sec.data.size());
    }
  }

  // Translate relocations directly into the output, patching only its copy
  // of the section bytes. Subsequent object emission and JIT mapping consume
  // the same unmodified assembler input.
  out.resize(util::align_up(out.size(), alignof(u32)));
  for (u32 si = 0; si < nsects; ++si) {
    const auto &sec = get_section(emit_secs[si]);
    if (sec.relocs.empty()) {
      continue;
    }
    layouts[si].reloc_off = u32(out.size());
    auto append_reloc = [&](relocation_info reloc) {
      const auto *bytes = reinterpret_cast<const u8 *>(&reloc);
      out.insert(out.end(), bytes, bytes + sizeof(reloc));
      ++layouts[si].nreloc;
    };
    for (const Relocation &reloc : sec.relocs) {
      // Translate to `(type, length, pcrel)` per the Mach-O ARM64 ABI.
      u32 type, length, pcrel;
      bool needs_addend_record = false;
      u32 reloc_kind = reloc.type;
      switch (reloc_kind) {
      case ARM64_RELOC_UNSIGNED:
        if (reloc.offset + sizeof(u64) > sec.data.size()) {
          TPDE_FATAL("absolute relocation outside section");
        }
        {
          const u64 addend = reloc.addend;
          std::memcpy(out.data() + layouts[si].file_off + reloc.offset,
                      &addend,
                      sizeof(addend));
        }
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
        SymRef target = reloc.symbol;
        i64 addend = reloc.addend;
        // An FDE refers to the function atom, not the first function in its
        // text section. Resolve section-relative function references to that
        // function's symbol before the linker splits subsections into atoms.
        if (sec.name == unsigned(SectionKind::EHFrame)) {
          SecRef target_sec = sym_section(target);
          u64 target_off = sym_ptr(target)->n_value + addend;
          if (auto it = symbols_at.find({target_sec.id(), target_off});
              it != symbols_at.end()) {
            target = it->second;
            addend = 0;
          }
        }
        i32 pre = i32(addend - reloc.offset);
        if (reloc.offset + sizeof(pre) > sec.data.size()) {
          TPDE_FATAL("PCREL32 relocation outside section");
        }
        std::memcpy(out.data() + layouts[si].file_off + reloc.offset,
                    &pre,
                    sizeof(pre));
        append_reloc(relocation_info{
            i32(reloc.offset),
            pack_reloc_word1(
                sym_to_table_idx[section_symbol(emit_secs[si]).id()],
                0,
                2,
                1,
                ARM64_RELOC_SUBTRACTOR)});
        append_reloc(relocation_info{
            i32(reloc.offset),
            pack_reloc_word1(
                sym_to_table_idx[target.id()], 0, 2, 1, ARM64_RELOC_UNSIGNED)});
        continue;
      }
      default:
        // Unknown framework relocation type — surface so upstream maps it.
        TPDE_FATAL("unsupported Mach-O relocation type");
      }

      u32 sym_table_idx = sym_to_table_idx[reloc.symbol.id()];

      if (needs_addend_record) {
        // 24-bit addend limit; flag larger ones.
        if (reloc.addend < -(1 << 23) || reloc.addend >= (1 << 23)) {
          TPDE_FATAL("ARM64_RELOC_ADDEND only supports 24-bit addends");
        }
        relocation_info ar{
            .r_address = i32(reloc.offset),
            .r_word1 = pack_reloc_word1(u32(reloc.addend) & 0xFFFFFFu,
                                        0,
                                        length,
                                        /*extern=*/0,
                                        ARM64_RELOC_ADDEND),
        };
        append_reloc(ar);
      }

      relocation_info ri{
          .r_address = i32(reloc.offset),
          .r_word1 = pack_reloc_word1(sym_table_idx,
                                      pcrel,
                                      length,
                                      /*extern=*/1,
                                      type),
      };
      append_reloc(ri);
    }
  }

  util::SmallVector<u8, 256> out_strtab;
  out_strtab.push_back(0);
  out.resize(util::align_up(out.size(), alignof(u64)));
  const u32 symoff = u32(out.size());
  for (SymRef ref : sym_order) {
    nlist_64 sym = *sym_ptr(ref);
    if ((sym.n_type & N_TYPE) == N_SECT) {
      sym.n_sect = u8(sec_to_ordinal[sym_section(ref).id()]);
      assert(sym.n_sect);
      sym.n_value += layouts[sym.n_sect - 1].addr;
    }
    if (sym.n_strx) {
      const char *name = sym_name(ref);
      sym.n_strx = u32(out_strtab.size());
      if (*name == '\1') {
        ++name;
      } else {
        out_strtab.push_back('_');
      }
      do {
        out_strtab.push_back(u8(*name));
      } while (*name++);
    }
    const auto *bytes = reinterpret_cast<const u8 *>(&sym);
    out.insert(out.end(), bytes, bytes + sizeof(sym));
  }
  const u32 stroff = u32(out.size());
  out.insert(out.end(), out_strtab.begin(), out_strtab.end());
  out.resize(util::align_up(out.size(), 8));
  const u32 strsize = u32(out.size() - stroff);

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
    seg->cmdsize =
        u32(sizeof(segment_command_64) + nsects * sizeof(section_64));
    set_seg_or_sect(seg->segname, "");
    seg->vmaddr = 0;
    seg->vmsize = segment_vmaddr;
    // file_off of first section's data = end of all load commands.
    // Compute `filesize` = sum of non-zerofill section sizes (with padding).
    u64 file_lo = ~u64(0), file_hi = 0;
    for (u32 si = 0; si < nsects; ++si) {
      DataSection &sec = get_section(emit_secs[si]);
      if (sec.is_virtual) {
        continue;
      }
      file_lo = std::min<u64>(file_lo, layouts[si].file_off);
      file_hi =
          std::max<u64>(file_hi, u64(layouts[si].file_off) + sec.data.size());
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

    auto *secs =
        reinterpret_cast<section_64 *>(lc + sizeof(segment_command_64));
    for (u32 si = 0; si < nsects; ++si) {
      DataSection &sec = get_section(emit_secs[si]);
      const auto &d = SECTION_DESCR[sec.name];
      auto &s = secs[si];
      set_seg_or_sect(s.sectname, d.sectname);
      set_seg_or_sect(s.segname, d.segname);
      s.addr = layouts[si].addr;
      s.size = sec.size();
      s.offset = layouts[si].file_off;
      s.align = std::countr_zero(sec.align);
      s.reloff = layouts[si].reloc_off;
      s.nreloc = layouts[si].nreloc;
      s.flags = sec.flags;
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
    bv->sdk = 0; // No SDK is selected by the object writer.
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
