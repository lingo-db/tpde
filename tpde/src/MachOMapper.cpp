// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/MachOMapper.hpp"

#include "tpde/Assembler.hpp"
#include "tpde/AssemblerMachO.hpp"
#include "tpde/MachO.hpp"
#include "tpde/base.hpp"
#include "tpde/util/SmallVector.hpp"
#include "tpde/util/misc.hpp"

#if defined(__APPLE__) && defined(__aarch64__)

  #include <algorithm>
  #include <compare>
  #include <cstring>
  #include <dlfcn.h>
  #include <libkern/OSCacheControl.h>
  #include <pthread.h>
  #include <sys/mman.h>
  #include <unistd.h>

  #include <disarm64.h>

  #include <atomic>
  #include <mutex>
  #include <vector>

namespace tpde::macho {

// ---------------------------------------------------------------------------
// libunwind dynamic-registration glue
// ---------------------------------------------------------------------------
//
// On modern macOS (~12.3+), libunwind exposes a callback API that lets a
// JIT advertise a compact-unwind (and optional eh_frame) section to the
// stack unwinder. When `_Unwind_RaiseException` (or `_Unwind_Backtrace`)
// hits a PC that isn't in any registered DSO, libunwind iterates the
// registered callbacks and asks each "do you know about this address?".
//
// Conventions worth knowing while reading the code:
//   - The compact-unwind entries we register store function_address as
//     **dso_base-relative offsets**, not absolute pointers (libunwind
//     adds dso_base when reading them). The framework's
//     `emit_compact_unwind_entry` writes absolute pointers via
//     ARM64_RELOC_UNSIGNED, so we walk the entries post-relocation and
//     subtract dso_base before registering. (LLVM ORC does the same.)
//   - Registration is per-process. A small global registry is consulted
//     by a single shared callback. Mapper instances add/remove entries
//     in their map()/reset(); the libunwind callback is registered
//     exactly once via std::call_once on first map().
//   - The dynamic API is weak-linked via dlsym so older systems that
//     lack it gracefully degrade. Without the API the unwinder cannot
//     walk through JIT'd frames — but for non-EH workloads that's a
//     soft failure (only matters when something tries to unwind).

namespace {

#pragma pack(push, 1)
struct UnwindDynamicSections {
  uintptr_t dso_base;
  uintptr_t dwarf_section;
  size_t dwarf_section_length;
  uintptr_t compact_unwind_section;
  size_t compact_unwind_section_length;
};
#pragma pack(pop)

using FindCallback = int (*)(uintptr_t, UnwindDynamicSections *);
using AddRemoveFn = int (*)(FindCallback);

// One entry per live `MachOMapper` that successfully registered. Sorted
// behavior isn't required — JIT counts are small and lookups are cold
// (only on the unwind path).
struct JitImage {
  uintptr_t text_start;
  uintptr_t text_end;
  uintptr_t dso_base;
  uintptr_t cu_addr;
  size_t cu_size;
};

std::mutex g_jit_images_mutex;
std::vector<JitImage> g_jit_images;

AddRemoveFn g_unw_add = nullptr;
AddRemoveFn g_unw_remove = nullptr;

// Diagnostic counter (testing-only): incremented every time the callback
// matches a registered JIT region. Exposed via a free function (see
// `tpde_macho_jit_unwind_hits` below) so the M4 test can confirm
// libunwind is actually consulting the dynamic-section callback rather
// than falling back to frame-pointer chain walking.
std::atomic<u64> g_unwind_callback_hits{0};

int find_unwind_for_jit_pc(uintptr_t addr, UnwindDynamicSections *out) {
  std::lock_guard<std::mutex> lock(g_jit_images_mutex);
  for (const auto &img : g_jit_images) {
    if (addr >= img.text_start && addr < img.text_end) {
      out->dso_base = img.dso_base;
      out->dwarf_section = 0; // No eh_frame fallback yet (M4 follow-up).
      out->dwarf_section_length = 0;
      out->compact_unwind_section = img.cu_addr;
      out->compact_unwind_section_length = img.cu_size;
      g_unwind_callback_hits.fetch_add(1, std::memory_order_relaxed);
      return 1;
    }
  }
  return 0;
}

std::once_flag g_unw_init_flag;
void init_unwind_callbacks_once() {
  // dlsym for the dynamic-section API. These are weak symbols on Apple
  // libunwind; older systems lack them entirely.
  g_unw_add = reinterpret_cast<AddRemoveFn>(
      ::dlsym(RTLD_DEFAULT, "__unw_add_find_dynamic_unwind_sections"));
  g_unw_remove = reinterpret_cast<AddRemoveFn>(
      ::dlsym(RTLD_DEFAULT, "__unw_remove_find_dynamic_unwind_sections"));
  if (g_unw_add) {
    g_unw_add(find_unwind_for_jit_pc);
  }
}

void register_jit_image(uintptr_t text_start, uintptr_t text_end,
                        uintptr_t dso_base, u8 *cu_data, size_t cu_size) {
  std::call_once(g_unw_init_flag, init_unwind_callbacks_once);
  if (!g_unw_add) {
    // Dynamic API missing on this system — silently degrade. Future
    // work: synthesize an `__eh_frame` blob and `__register_frame`
    // each FDE for older macOS.
    return;
  }
  std::lock_guard<std::mutex> lock(g_jit_images_mutex);
  g_jit_images.push_back({text_start, text_end, dso_base,
                          uintptr_t(cu_data), cu_size});
}

void unregister_jit_image(uintptr_t text_start) {
  if (!g_unw_add) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_jit_images_mutex);
  for (auto it = g_jit_images.begin(); it != g_jit_images.end(); ++it) {
    if (it->text_start == text_start) {
      g_jit_images.erase(it);
      return;
    }
  }
}

// Bit blend, identical to the helper in ElfMapper — overwrite the bits in
// `mask` of the 32-bit instruction at `pc` with the matching bits of `data`.
inline void blend(uintptr_t pc, u32 mask, u32 data) {
  u32 *dest = reinterpret_cast<u32 *>(pc);
  *dest = (data & mask) | (*dest & ~mask);
}

// Permission classification for a section. We split the JIT region into RX
// and RW(+BSS) blocks so the same `MAP_JIT` allocation can hold both.
enum class Perm : u8 { RX, RW, BSS };

Perm perm_of_section(const DataSection &sec) {
  if (sec.is_virtual) {
    return Perm::BSS;
  }
  // Mach-O packs section type|attrs into `flags`. Pure-instructions => RX.
  if (sec.flags & S_ATTR_PURE_INSTRUCTIONS) {
    return Perm::RX;
  }
  return Perm::RW;
}

} // anonymous namespace

// Test-only accessor for `g_unwind_callback_hits`. Defined out of line and
// non-templated so the symbol is straightforward to reference from
// outside the translation unit without visibility/linkage friction.
u64 tpde_macho_jit_unwind_hits() {
  return g_unwind_callback_hits.load(std::memory_order_relaxed);
}

void MachOMapper::reset() {
  if (!mapped_addr) {
    return;
  }
  if (cu_section_addr) {
    unregister_jit_image(reinterpret_cast<uintptr_t>(mapped_addr));
    cu_section_addr = nullptr;
    cu_section_size = 0;
  }
  ::munmap(mapped_addr, mapped_size);
  mapped_addr = nullptr;
  mapped_size = 0;
  sym_addrs.clear();
}

bool MachOMapper::map(AssemblerMachO &assembler, SymbolResolver resolver) {
  // ----- Step 1: enumerate sections to allocate, sorted by permission so
  // the resulting layout is `[RX...] [RW...] [BSS...]`. We collect every
  // section that participates in execution / data; empty sections (no data,
  // no vsize) are skipped to keep the layout tight.
  struct AllocSec {
    SecRef ref;
    Perm perm;
    u32 sort_key;
    std::weak_ordering operator<=>(const AllocSec &o) const {
      return sort_key <=> o.sort_key;
    }
  };
  util::SmallVector<AllocSec, 8> alloc;
  for (size_t i = 0; i < assembler.sections.size(); ++i) {
    if (!assembler.sections[i]) {
      continue;
    }
    const DataSection &sec = *assembler.sections[i];
    if (sec.size() == 0) {
      continue;
    }
    Perm p = perm_of_section(sec);
    u32 key = u32(p);
    alloc.push_back({SecRef(u32(i)), p, key});
  }
  std::stable_sort(alloc.begin(), alloc.end());

  // ----- Step 2: assign offsets within the JIT region. We need PLT/GOT
  // trampoline space for ±128 MiB-out-of-range branches; conservatively
  // reserve 16B per externally-resolved symbol up front (in the RX block).
  constexpr size_t PLT_ENTRY_SIZE = 16;
  // Approximate; mirrors ElfMapper's coarse upper bound.
  u32 plt_slot_count =
      u32(assembler.local_symbols.size() + assembler.global_symbols.size());

  size_t page_size = size_t(::getpagesize());

  size_t off = 0;
  size_t plt_region_off = 0;
  size_t plt_region_size = 0;
  if (plt_slot_count) {
    plt_region_off = 0;
    plt_region_size = size_t(plt_slot_count) * PLT_ENTRY_SIZE;
    off = plt_region_size;
  }

  // Permission boundaries: pairs of (start_offset, perm) so we can flip
  // memory protections after copying. Currently MAP_JIT keeps everything
  // RX — the data segment is also RX-protected post-flush, but that's fine
  // for read-mostly relocs and for BSS we just leave the bytes zeroed.
  struct Boundary {
    size_t start;
    Perm perm;
  };
  util::SmallVector<Boundary, 4> boundaries;
  if (plt_slot_count) {
    boundaries.push_back({plt_region_off, Perm::RX});
  }

  Perm prev = plt_slot_count ? Perm::RX : Perm::BSS; // anything that won't
                                                     // match Perm::RX
  for (auto &as : alloc) {
    DataSection &sec = assembler.get_section(as.ref);
    u32 sec_align = sec.align ? sec.align : 1;
    if (sec_align > page_size) {
      // mmap only hands out page-aligned regions; we can't honor this.
      sec_align = u32(page_size);
    }
    if (boundaries.empty() || as.perm != prev) {
      // Round up to a page when permissions change so mprotect can work
      // without splitting pages. (Even though we don't actually mprotect
      // RW separately right now, keep the layout future-proof.)
      off = util::align_up(off, page_size);
      boundaries.push_back({off, as.perm});
      prev = as.perm;
    } else {
      off = util::align_up(off, sec_align);
    }
    sec.addr = off;
    off += sec.size();
  }
  // Pad to page so the JIT region's tail isn't shared with anything.
  off = util::align_up(off, page_size);

  mapped_size = off ? off : page_size;

  // ----- Step 3: allocate `MAP_JIT` memory. On Apple Silicon, `MAP_JIT`
  // requires the host binary be at least ad-hoc-signed; the toggle to
  // `pthread_jit_write_protect_np(0)` then makes the region writable for
  // the calling thread.
  //
  // Note: MAP_JIT is the only portable way to get an RWX-capable region on
  // Apple Silicon (kernel forbids `mprotect` on JIT pages with the
  // hardened runtime). We write under a pthread protection window and flip
  // back to executable mode before returning.
  void *p = ::mmap(nullptr, mapped_size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (p == MAP_FAILED) {
    // Fallback: try without MAP_JIT in case we're not signed (this won't
    // succeed under hardened runtime but is fine for plain CLI tests).
    p = ::mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
      mapped_addr = nullptr;
      return false;
    }
  }
  mapped_addr = static_cast<u8 *>(p);

  // Flip into write mode for this thread before copying section data.
  ::pthread_jit_write_protect_np(0);

  bool success = true;

  // ----- Step 4: resolve symbol addresses. Locals first, then globals, in
  // the same order AssemblerMachO stores them.
  local_sym_count = u32(assembler.local_symbols.size());
  sym_addrs.resize(local_sym_count + assembler.global_symbols.size());

  auto sym_storage_idx = [&](SymRef sym) -> size_t {
    return AssemblerMachO::sym_is_local(sym)
               ? AssemblerMachO::sym_idx(sym)
               : (AssemblerMachO::sym_idx(sym) + local_sym_count);
  };

  auto sym_addr = [&](SymRef sym) -> void * {
    // Synthetic section-symbol SymRefs aren't in `sym_addrs`; resolve them
    // directly via the section's mapped base. They're produced by
    // `AssemblerMachO::section_symbol(SecRef)` to anchor PCREL32 records
    // without polluting the symbol table.
    if (AssemblerMachO::is_section_sym(sym)) {
      SecRef sr = AssemblerMachO::section_sym_to_secref(sym);
      return mapped_addr + assembler.get_section(sr).addr;
    }
    size_t idx = sym_storage_idx(sym);
    if (sym_addrs[idx]) {
      return sym_addrs[idx];
    }
    const nlist_64 *ns = assembler.sym_ptr(sym);
    if ((ns->n_type & N_TYPE) == N_UNDF) {
      // Undefined external — ask the resolver. dlsym wants the un-prefixed
      // name on Darwin (libdl synthesizes the underscore).
      const char *name = assembler.sym_name(sym);
      void *addr = nullptr;
      if (name && *name) {
        addr = resolver(name);
        if (!addr) {
          // Fallback: dlsym(RTLD_DEFAULT, name).
          addr = ::dlsym(RTLD_DEFAULT, name);
        }
      }
      if (!addr && (ns->n_type & N_EXT)) {
        TPDE_LOG_ERR("unresolved Mach-O symbol {}",
                     name ? name : "<unnamed>");
        success = false;
      }
      sym_addrs[idx] = addr;
    } else if ((ns->n_type & N_TYPE) == N_SECT) {
      SecRef sec = assembler.sym_section(sym);
      if (!sec.valid()) {
        sym_addrs[idx] = nullptr;
      } else {
        const auto &dsec = assembler.get_section(sec);
        sym_addrs[idx] = mapped_addr + dsec.addr + ns->n_value;
      }
    } else if ((ns->n_type & N_TYPE) == N_ABS) {
      sym_addrs[idx] = reinterpret_cast<void *>(ns->n_value);
    }
    return sym_addrs[idx];
  };

  // Pre-resolve all defined globals so `get_sym_addr` works after map().
  for (u32 i = 0; i < assembler.global_symbols.size(); ++i) {
    if ((assembler.global_symbols[i].n_type & N_TYPE) != N_UNDF) {
      (void)sym_addr(SymRef(i | 0x8000'0000u));
    }
  }

  // ----- Step 5: PLT/GOT trampoline allocator.
  // Mach-O ARM64 BRANCH26 has a ±128 MiB reach. For symbols outside that
  // range (typically anything in libSystem) we synthesize a small thunk:
  //   ldr  x16, .+8
  //   br   x16
  //   .quad <abs_target>
  // Identical math to the AArch64 trampoline in ElfMapper.
  u8 *next_plt_entry = mapped_addr + plt_region_off;
  util::SmallVector<u8 *, 16> got_plt_slots;
  got_plt_slots.resize(sym_addrs.size());
  auto plt_entry = [&](size_t idx, uintptr_t addr) -> uintptr_t {
    if (!got_plt_slots[idx]) {
      *reinterpret_cast<u32 *>(next_plt_entry + 0 * sizeof(u32)) =
          de64_LDRx_pcrel(DA_GP(16), 2);
      *reinterpret_cast<u32 *>(next_plt_entry + 1 * sizeof(u32)) =
          de64_BR(DA_GP(16));
      *reinterpret_cast<uintptr_t *>(next_plt_entry + sizeof(uintptr_t)) =
          addr;
      assert(plt_slot_count-- > 0 && "insufficient PLT/GOT slots");
      got_plt_slots[idx] = next_plt_entry;
      next_plt_entry += PLT_ENTRY_SIZE;
    }
    return reinterpret_cast<uintptr_t>(got_plt_slots[idx]);
  };

  // ----- Step 6: copy section bytes and apply relocations.
  for (auto &as : alloc) {
    DataSection &sec = assembler.get_section(as.ref);
    u8 *dst = mapped_addr + sec.addr;
    if (!sec.is_virtual) {
      std::memcpy(dst, sec.data.data(), sec.data.size());
    } else {
      // BSS/zerofill; mmap zero-initialized us already.
    }

    for (auto &reloc : sec.relocs) {
      uintptr_t pc = uintptr_t(dst + reloc.offset);
      void *sym_p = sym_addr(reloc.symbol);
      uintptr_t sym = uintptr_t(sym_p);
      uintptr_t syma = sym + reloc.addend;

      switch (reloc.type) {
      case ARM64_RELOC_UNSIGNED: {
        u64 v = syma;
        std::memcpy(reinterpret_cast<void *>(pc), &v, sizeof(u64));
        break;
      }
      case ARM64_RELOC_TPDE_PCREL32: {
        // Synthetic `target - pc` 32-bit diff. The pre-image at `pc`
        // already holds whatever the framework wrote (often 0). We
        // *replace* it with the resolved diff.
        intptr_t v = intptr_t(syma) - intptr_t(pc);
        if (util::sext(u64(v), 32) != intptr_t(v)) {
          TPDE_LOG_ERR("ARM64_RELOC_TPDE_PCREL32 out of range: {:#x}",
                       u64(v));
          success = false;
        }
        u32 v32 = u32(v);
        std::memcpy(reinterpret_cast<void *>(pc), &v32, sizeof(u32));
        break;
      }
      case ARM64_RELOC_BRANCH26: {
        intptr_t v = intptr_t(syma) - intptr_t(pc);
        if ((v & 3) || util::sext(u64(v), 28) != intptr_t(v)) {
          v = intptr_t(plt_entry(sym_storage_idx(reloc.symbol), sym)) +
              reloc.addend - intptr_t(pc);
        }
        if ((v & 3) || util::sext(u64(v), 28) != intptr_t(v)) {
          TPDE_LOG_ERR("ARM64_RELOC_BRANCH26 out of range");
          success = false;
        }
        blend(pc, 0x03ff'ffffu, u32(v >> 2));
        break;
      }
      case ARM64_RELOC_PAGE21: {
        intptr_t v = intptr_t(util::align_down(syma, 0x1000)) -
                     intptr_t(util::align_down(pc, 0x1000));
        if (util::sext(u64(v), 33) != intptr_t(v)) {
          TPDE_LOG_ERR("ARM64_RELOC_PAGE21 out of range");
          success = false;
        }
        v >>= 12;
        blend(pc, 0x60ff'ffe0u,
              u32(((v & 3) << 29) | (((v >> 2) & 0x7'ffff) << 5)));
        break;
      }
      case ARM64_RELOC_PAGEOFF12: {
        // ARM64 PAGEOFF12 implicit shift depends on the patched opcode.
        // For ADD #imm12 the shift is 0; for LDR/STR the shift matches
        // the memory-access size. Inspect the instruction in place.
        u32 inst = *reinterpret_cast<u32 *>(pc);
        u32 shift = 0;
        if ((inst & 0xff00'0000u) == 0x91000000u) {
          // ADD (immediate, 64-bit) — shift 0.
          shift = 0;
        } else if ((inst & 0x3b00'0000u) == 0x39000000u) {
          // LDR/STR (unsigned offset, integer). Size is in bits 30..31.
          u32 size = (inst >> 30) & 3;
          shift = size; // bytes log2
        } else if ((inst & 0x3f00'0000u) == 0x3d000000u) {
          // LDR/STR (FP/SIMD, unsigned offset). Size in bits 30..31 +
          // opc<23>.
          u32 size = (inst >> 30) & 3;
          u32 opc1 = (inst >> 23) & 1;
          shift = (opc1 << 2) | size;
        }
        u32 v = u32((syma & 0xfffu) >> shift) << 10;
        blend(pc, 0xfffu << 10, v);
        break;
      }
      default:
        TPDE_LOG_ERR("unsupported Mach-O relocation type: {:#x}",
                     reloc.type);
        success = false;
      }
    }
  }

  // ----- Step 6.5: prepare the compact-unwind section for libunwind
  // dynamic registration. The .o-style entries the assembler emitted use
  // ARM64_RELOC_UNSIGNED for `function_address`, so after relocation
  // those slots hold absolute pointers. libunwind's dynamic-section API
  // wants them as **dso_base-relative offsets**: the unwinder adds
  // `dso_base` (which we set to `mapped_addr`) when consulting the
  // entry. Walk every 32-byte entry and rewrite function_address (and
  // personality/lsda once those are non-zero in M4 follow-up).
  //
  // We also remember the section's mapped location so `reset()` can
  // unregister via the cached `text_start` key.
  for (size_t i = 0; i < assembler.sections.size(); ++i) {
    if (!assembler.sections[i]) {
      continue;
    }
    DataSection &sec = *assembler.sections[i];
    if (sec.size() == 0) {
      continue;
    }
    // The (segname, sectname) names live in the SECTION_DESCR table by
    // index; the simplest discriminator we have here is the original
    // SectionKind value via the `name` field, which AssemblerMachO sets
    // to `unsigned(SectionKind)`. The `__compact_unwind` slot is index
    // `SectionKind::CompactUnwind`.
    if (sec.name != unsigned(SectionKind::CompactUnwind)) {
      continue;
    }
    cu_section_addr = mapped_addr + sec.addr;
    cu_section_size = sec.size();
    // Walk 32-byte entries. function_address is at offset 0.
    constexpr size_t kEntrySize = 32;
    assert(cu_section_size % kEntrySize == 0 &&
           "compact_unwind section size must be a multiple of 32");
    uintptr_t dso_base = reinterpret_cast<uintptr_t>(mapped_addr);
    for (size_t off = 0; off < cu_section_size; off += kEntrySize) {
      u8 *entry = cu_section_addr + off;
      uintptr_t abs;
      std::memcpy(&abs, entry, sizeof(uintptr_t));
      uintptr_t rel = abs - dso_base;
      std::memcpy(entry, &rel, sizeof(uintptr_t));
      // Personality (offset 16) and LSDA (offset 24) are zero today;
      // when M4 follow-up wires them up they'll need the same fix-up.
    }
    break; // only one compact_unwind section per assembler
  }

  // ----- Step 7: flip back to execute mode and invalidate the I-cache.
  ::pthread_jit_write_protect_np(1);
  ::sys_icache_invalidate(mapped_addr, mapped_size);

  if (!success) {
    reset();
    return false;
  }

  // ----- Step 8: register with libunwind. Conservatively use the whole
  // mapped region as the "text" range — libunwind only consults the
  // callback on actual unwind, and we don't have a tighter bound that
  // covers all RX sub-regions cheaply. Mismatched lookups (e.g., for
  // PLT trampoline addresses) just fall through to "no info", which is
  // correct: the trampolines are leaf-and-tail-call so the unwinder
  // doesn't try to step out of them.
  if (cu_section_addr) {
    register_jit_image(reinterpret_cast<uintptr_t>(mapped_addr),
                       reinterpret_cast<uintptr_t>(mapped_addr + mapped_size),
                       reinterpret_cast<uintptr_t>(mapped_addr),
                       cu_section_addr, cu_section_size);
  }

  return true;
}

void *MachOMapper::get_sym_addr(SymRef sym) const {
  size_t idx = AssemblerMachO::sym_is_local(sym)
                   ? AssemblerMachO::sym_idx(sym)
                   : (AssemblerMachO::sym_idx(sym) + local_sym_count);
  assert(idx < sym_addrs.size());
  return sym_addrs[idx];
}

std::pair<void *, size_t> MachOMapper::get_mapped_range() const {
  return {mapped_addr, mapped_size};
}

} // namespace tpde::macho

#else // !(__APPLE__ && __aarch64__)

namespace tpde::macho {
void MachOMapper::reset() {}
bool MachOMapper::map(AssemblerMachO &, SymbolResolver) { return false; }
void *MachOMapper::get_sym_addr(SymRef) const { return nullptr; }
std::pair<void *, size_t> MachOMapper::get_mapped_range() const {
  return {nullptr, 0ull};
}
} // namespace tpde::macho

#endif
