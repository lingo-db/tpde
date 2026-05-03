// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include "tpde/base.hpp"

// Wire-format definitions for Mach-O 64 (MH_OBJECT). Mirrors what
// `<mach-o/loader.h>`, `<mach-o/nlist.h>`, `<mach-o/reloc.h>`, and
// `<mach-o/arm64/reloc.h>` define on Apple platforms, plus a handful of
// compact-unwind constants from `<mach-o/compact_unwind_encoding.h>`. Kept
// self-contained so the assembler builds on non-Apple hosts without pulling
// in Darwin SDK headers.

namespace tpde::macho {

// ----- mach_header_64 magic / cpu types -----
constexpr u32 MH_MAGIC_64 = 0xFEEDFACFu;
constexpr i32 CPU_TYPE_ARM64 = 0x0100000C; // CPU_TYPE_ARM | CPU_ARCH_ABI64
constexpr i32 CPU_SUBTYPE_ARM64_ALL = 0;

constexpr u32 MH_OBJECT = 0x1u;
constexpr u32 MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000u;

// ----- Load command identifiers -----
constexpr u32 LC_SEGMENT_64 = 0x19u;
constexpr u32 LC_SYMTAB = 0x02u;
constexpr u32 LC_DYSYMTAB = 0x0Bu;
constexpr u32 LC_BUILD_VERSION = 0x32u;

constexpr u32 PLATFORM_MACOS = 1u;

// ----- Section types (low 8 bits of section.flags) -----
constexpr u32 SECTION_TYPE = 0x000000FFu;
constexpr u32 SECTION_ATTRIBUTES = 0xFFFFFF00u;

constexpr u32 S_REGULAR = 0x00u;
constexpr u32 S_ZEROFILL = 0x01u;
constexpr u32 S_CSTRING_LITERALS = 0x02u;
constexpr u32 S_COALESCED = 0x0Bu;
constexpr u32 S_THREAD_LOCAL_REGULAR = 0x11u;
constexpr u32 S_THREAD_LOCAL_ZEROFILL = 0x12u;

// ----- Section attributes (high 24 bits of section.flags) -----
constexpr u32 S_ATTR_PURE_INSTRUCTIONS = 0x80000000u;
constexpr u32 S_ATTR_NO_DEAD_STRIP = 0x10000000u;
constexpr u32 S_ATTR_LIVE_SUPPORT = 0x08000000u;
constexpr u32 S_ATTR_SOME_INSTRUCTIONS = 0x00000400u;

// ----- nlist_64 n_type fields -----
constexpr u8 N_STAB = 0xE0u;
constexpr u8 N_PEXT = 0x10u;
constexpr u8 N_TYPE = 0x0Eu;
constexpr u8 N_EXT = 0x01u;

constexpr u8 N_UNDF = 0x0u;
constexpr u8 N_ABS = 0x2u;
constexpr u8 N_SECT = 0xEu;

constexpr u16 N_NO_DEAD_STRIP = 0x0020u;
constexpr u16 N_WEAK_DEF = 0x0080u;

// ----- ARM64 relocation types -----
enum ARM64Reloc : u32 {
  ARM64_RELOC_UNSIGNED = 0,
  ARM64_RELOC_SUBTRACTOR = 1,
  ARM64_RELOC_BRANCH26 = 2,
  ARM64_RELOC_PAGE21 = 3,
  ARM64_RELOC_PAGEOFF12 = 4,
  ARM64_RELOC_GOT_LOAD_PAGE21 = 5,
  ARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6,
  ARM64_RELOC_POINTER_TO_GOT = 7,
  ARM64_RELOC_TLVP_LOAD_PAGE21 = 8,
  ARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9,
  ARM64_RELOC_ADDEND = 10,
};

// Synthetic relocation kind used internally by TPDE on Mach-O for the
// "32-bit pc-relative" hook used by `Assembler::reloc_pc32`. The framework
// emits these into the EH frame section to point FDEs at functions; the
// Mach-O object writer expands them to a SUBTRACTOR+UNSIGNED pair, and the
// JIT mapper resolves them as a plain `target - pc` 32-bit difference.
// Picked outside the standard 0..10 ARM64 enum range so it never clashes.
constexpr u32 ARM64_RELOC_TPDE_PCREL32 = 0x100u;

// ----- Compact unwind encoding (ARM64) -----
constexpr u32 UNWIND_ARM64_MODE_FRAME = 0x04000000u;
constexpr u32 UNWIND_ARM64_MODE_DWARF = 0x03000000u;
constexpr u32 UNWIND_HAS_LSDA = 0x40000000u;

constexpr u32 UNWIND_ARM64_FRAME_X19_X20_PAIR = 0x00000001u;
constexpr u32 UNWIND_ARM64_FRAME_X21_X22_PAIR = 0x00000002u;
constexpr u32 UNWIND_ARM64_FRAME_X23_X24_PAIR = 0x00000004u;
constexpr u32 UNWIND_ARM64_FRAME_X25_X26_PAIR = 0x00000008u;
constexpr u32 UNWIND_ARM64_FRAME_X27_X28_PAIR = 0x00000010u;
constexpr u32 UNWIND_ARM64_FRAME_D8_D9_PAIR = 0x00000100u;
constexpr u32 UNWIND_ARM64_FRAME_D10_D11_PAIR = 0x00000200u;
constexpr u32 UNWIND_ARM64_FRAME_D12_D13_PAIR = 0x00000400u;
constexpr u32 UNWIND_ARM64_FRAME_D14_D15_PAIR = 0x00000800u;

// ----- On-disk structures -----
#pragma pack(push, 1)

struct mach_header_64 {
  u32 magic;
  i32 cputype;
  i32 cpusubtype;
  u32 filetype;
  u32 ncmds;
  u32 sizeofcmds;
  u32 flags;
  u32 reserved;
};

struct segment_command_64 {
  u32 cmd;
  u32 cmdsize;
  char segname[16];
  u64 vmaddr;
  u64 vmsize;
  u64 fileoff;
  u64 filesize;
  i32 maxprot;
  i32 initprot;
  u32 nsects;
  u32 flags;
};

struct section_64 {
  char sectname[16];
  char segname[16];
  u64 addr;
  u64 size;
  u32 offset;
  u32 align;
  u32 reloff;
  u32 nreloc;
  u32 flags;
  u32 reserved1;
  u32 reserved2;
  u32 reserved3;
};

struct build_version_command {
  u32 cmd;
  u32 cmdsize;
  u32 platform;
  u32 minos;
  u32 sdk;
  u32 ntools;
};

struct symtab_command {
  u32 cmd;
  u32 cmdsize;
  u32 symoff;
  u32 nsyms;
  u32 stroff;
  u32 strsize;
};

struct dysymtab_command {
  u32 cmd;
  u32 cmdsize;
  u32 ilocalsym;
  u32 nlocalsym;
  u32 iextdefsym;
  u32 nextdefsym;
  u32 iundefsym;
  u32 nundefsym;
  u32 tocoff;
  u32 ntoc;
  u32 modtaboff;
  u32 nmodtab;
  u32 extrefsymoff;
  u32 nextrefsyms;
  u32 indirectsymoff;
  u32 nindirectsyms;
  u32 extreloff;
  u32 nextrel;
  u32 locreloff;
  u32 nlocrel;
};

struct nlist_64 {
  u32 n_strx;
  u8 n_type;
  u8 n_sect;
  u16 n_desc;
  u64 n_value;
};

struct relocation_info {
  i32 r_address;
  // Packed bitfield (LE on disk):
  //   bits  0..23 r_symbolnum
  //   bit  24     r_pcrel
  //   bits 25..26 r_length
  //   bit  27     r_extern
  //   bits 28..31 r_type
  u32 r_word1;
};

#pragma pack(pop)

constexpr u32 pack_reloc_word1(u32 symbolnum,
                               u32 pcrel,
                               u32 length,
                               u32 extern_,
                               u32 type) {
  return (symbolnum & 0xFFFFFFu) | ((pcrel & 1) << 24) |
         ((length & 3) << 25) | ((extern_ & 1) << 27) | ((type & 0xFu) << 28);
}

} // namespace tpde::macho
