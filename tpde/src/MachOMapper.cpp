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
  #include <cstring>
  #include <libkern/OSCacheControl.h>
  #include <pthread.h>
  #include <sys/mman.h>
  #include <unistd.h>

  #include <disarm64.h>

extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);

namespace tpde::macho {
namespace {

// Bit blend, identical to the helper in ElfMapper — overwrite the bits in
// `mask` of the 32-bit instruction at `pc` with the matching bits of `data`.
inline void blend(uintptr_t pc, u32 mask, u32 data) {
  u32 *dest = reinterpret_cast<u32 *>(pc);
  *dest = (data & mask) | (*dest & ~mask);
}

bool is_code(const DataSection &sec) {
  return !sec.is_virtual && (sec.flags & S_ATTR_PURE_INSTRUCTIONS);
}

} // anonymous namespace

void MachOMapper::reset() {
  for (size_t i = destructors.size(); i != 0; --i) {
    destructors[i - 1]();
  }
  destructors.clear();
  for (void *fde : registered_fdes) {
    __deregister_frame(fde);
  }
  registered_fdes.clear();
  if (mapped_addr) {
    ::munmap(mapped_addr, mapped_size);
  }
  if (data_addr) {
    ::munmap(data_addr, data_size);
  }
  mapped_addr = data_addr = nullptr;
  mapped_size = data_size = 0;
  sym_addrs.clear();
}

bool MachOMapper::map(AssemblerMachO &assembler, SymbolResolver resolver) {
  reset();
  util::SmallVector<DataSection *, 8> alloc;
  for (const auto &sec : assembler.sections) {
    if (sec) {
      alloc.push_back(sec.get());
    }
  }

  // Reserve one PLT/GOT slot per symbol, plus slots for branch addends.
  // Code and writable data have independent offsets and allocations.
  constexpr size_t PLT_ENTRY_SIZE = 16;
  // Approximate; mirrors ElfMapper's coarse upper bound.
  u32 plt_slot_count = u32(assembler.symbols.size());

  const u32 first_extra_slot = plt_slot_count;
  for (const auto &as : alloc) {
    for (const auto &reloc : as->relocs) {
      if (reloc.type == ARM64_RELOC_BRANCH26 && reloc.addend) {
        ++plt_slot_count;
      }
    }
  }
  size_t page_size = size_t(::getpagesize());

  size_t code_size = size_t(plt_slot_count) * PLT_ENTRY_SIZE;
  data_size = 0;
  for (const auto &as : alloc) {
    auto &sec = *as;
    if (sec.align > page_size) {
      TPDE_LOG_ERR("Mach-O JIT section alignment exceeds page size");
      reset();
      return false;
    }
    auto &size = is_code(sec) ? code_size : data_size;
    size = util::align_up(size, std::max<u32>(sec.align, 1));
    sec.addr = size;
    size += std::max<size_t>(sec.size(), 1);
  }
  mapped_size = util::align_up(std::max(code_size, page_size), page_size);
  data_size = util::align_up(std::max(data_size, page_size), page_size);
  void *p = ::mmap(nullptr,
                   mapped_size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1,
                   0);
  if (p == MAP_FAILED) {
    reset();
    return false;
  }
  mapped_addr = static_cast<u8 *>(p);
  // A nearby hint normally keeps data within ADRP/PCREL32 reach. Relocation
  // range checks below reject allocations that the kernel placed too far away.
  p = ::mmap(mapped_addr + mapped_size,
             data_size,
             PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON,
             -1,
             0);
  if (p == MAP_FAILED) {
    reset();
    return false;
  }
  data_addr = static_cast<u8 *>(p);
  for (const auto &as : alloc) {
    auto &sec = *as;
    sec.addr += uintptr_t(is_code(sec) ? mapped_addr : data_addr);
  }

  bool success = true;

  // Symbol addresses use the assembler's stable IDs.
  sym_addrs.resize(assembler.symbols.size());

  auto sym_addr = [&](SymRef sym) -> void * {
    size_t idx = sym.id();
    if (sym_addrs[idx]) {
      return sym_addrs[idx];
    }
    const nlist_64 *ns = assembler.sym_ptr(sym);
    if ((ns->n_type & N_TYPE) == N_UNDF) {
      // The supplied resolver owns external symbol lookup policy.
      const char *name = assembler.sym_name(sym);
      void *addr = name && *name ? resolver(name) : nullptr;
      if (!addr && (ns->n_type & N_EXT) && !(ns->n_desc & N_WEAK_REF)) {
        TPDE_LOG_ERR("unresolved Mach-O symbol {}", name ? name : "<unnamed>");
        success = false;
      }
      sym_addrs[idx] = addr;
    } else if ((ns->n_type & N_TYPE) == N_SECT) {
      SecRef sec = assembler.sym_section(sym);
      if (!sec.valid()) {
        sym_addrs[idx] = nullptr;
      } else {
        const auto &dsec = assembler.get_section(sec);
        sym_addrs[idx] = reinterpret_cast<void *>(dsec.addr + ns->n_value);
      }
    } else if ((ns->n_type & N_TYPE) == N_ABS) {
      sym_addrs[idx] = reinterpret_cast<void *>(ns->n_value);
    }
    return sym_addrs[idx];
  };

  // Resolve before disabling execute access: a resolver may itself run JIT
  // code.
  for (u32 i = 1; i < assembler.symbols.size(); ++i) {
    if ((assembler.symbols[i].entry.n_type & N_TYPE) != N_UNDF) {
      (void)sym_addr(SymRef(i));
    }
  }
  for (const auto &as : alloc) {
    for (const auto &reloc : as->relocs) {
      (void)sym_addr(reloc.symbol);
    }
  }
  if (!success) {
    reset();
    return false;
  }

  // PLT/GOT trampoline allocator.
  // Mach-O ARM64 BRANCH26 has a ±128 MiB reach. For symbols outside that
  // range (typically anything in libSystem) we synthesize a small thunk:
  //   ldr  x16, .+8
  //   br   x16
  //   .quad <abs_target>
  // Identical math to the AArch64 trampoline in ElfMapper.
  u8 *next_plt_entry = mapped_addr;
  util::SmallVector<u8 *, 16> got_plt_slots;
  got_plt_slots.resize(plt_slot_count);
  u32 extra_slot = first_extra_slot;
  auto plt_entry = [&](size_t idx, uintptr_t addr) -> uintptr_t {
    if (!got_plt_slots[idx]) {
      *reinterpret_cast<u32 *>(next_plt_entry + 0 * sizeof(u32)) =
          de64_LDRx_pcrel(DA_GP(16), 2);
      *reinterpret_cast<u32 *>(next_plt_entry + 1 * sizeof(u32)) =
          de64_BR(DA_GP(16));
      *reinterpret_cast<uintptr_t *>(next_plt_entry + sizeof(uintptr_t)) = addr;
      assert(plt_slot_count-- > 0 && "insufficient PLT/GOT slots");
      got_plt_slots[idx] = next_plt_entry;
      next_plt_entry += PLT_ENTRY_SIZE;
    }
    return reinterpret_cast<uintptr_t>(got_plt_slots[idx]);
  };

  ::pthread_jit_write_protect_np(0);

  // Copy section bytes and apply relocations.
  for (auto &as : alloc) {
    DataSection &sec = *as;
    u8 *dst = reinterpret_cast<u8 *>(sec.addr);
    if (!sec.is_virtual) {
      std::memcpy(dst, sec.data.data(), sec.data.size());
    }

    for (auto &reloc : sec.relocs) {
      uintptr_t pc = uintptr_t(dst + reloc.offset);
      void *sym_p = sym_addrs[reloc.symbol.id()];
      uintptr_t sym = uintptr_t(sym_p);
      uintptr_t syma = sym + reloc.addend;

      // A GOT slot is the literal in the symbol's branch trampoline.
      if (reloc.type == ARM64_RELOC_GOT_LOAD_PAGE21 ||
          reloc.type == ARM64_RELOC_GOT_LOAD_PAGEOFF12) {
        syma = plt_entry(reloc.symbol.id(), sym) + 8 + reloc.addend;
      }
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
          TPDE_LOG_ERR("ARM64_RELOC_TPDE_PCREL32 out of range: {:#x}", u64(v));
          success = false;
        }
        u32 v32 = u32(v);
        std::memcpy(reinterpret_cast<void *>(pc), &v32, sizeof(u32));
        break;
      }
      case ARM64_RELOC_BRANCH26: {
        intptr_t v = intptr_t(syma) - intptr_t(pc);
        if (!(syma & 3) && util::sext(u64(v), 28) != intptr_t(v)) {
          const size_t slot = reloc.addend ? extra_slot++ : reloc.symbol.id();
          v = intptr_t(plt_entry(slot, syma)) - intptr_t(pc);
        }
        if ((v & 3) || util::sext(u64(v), 28) != intptr_t(v)) {
          TPDE_LOG_ERR("ARM64_RELOC_BRANCH26 out of range");
          success = false;
        }
        blend(pc, 0x03ff'ffffu, u32(v >> 2));
        break;
      }
      case ARM64_RELOC_GOT_LOAD_PAGE21:
      case ARM64_RELOC_PAGE21: {
        intptr_t v = intptr_t(util::align_down(syma, 0x1000)) -
                     intptr_t(util::align_down(pc, 0x1000));
        if (util::sext(u64(v), 33) != intptr_t(v)) {
          TPDE_LOG_ERR("ARM64_RELOC_PAGE21 out of range");
          success = false;
        }
        v >>= 12;
        blend(pc,
              0x60ff'ffe0u,
              u32(((v & 3) << 29) | (((v >> 2) & 0x7'ffff) << 5)));
        break;
      }
      case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
      case ARM64_RELOC_PAGEOFF12: {
        // ARM64 PAGEOFF12 implicit shift depends on the patched opcode.
        // For ADD #imm12 the shift is 0; for LDR/STR the shift matches
        // the memory-access size. Inspect the instruction in place.
        u32 inst = *reinterpret_cast<u32 *>(pc);
        u32 shift = 0;
        if ((inst & 0xff00'0000u) == 0x91000000u) {
          // ADD (immediate, 64-bit) — shift 0.
          shift = 0;
        } else if ((inst & 0x3f00'0000u) == 0x39000000u) {
          // LDR/STR (unsigned offset, integer). Size is in bits 30..31.
          u32 size = (inst >> 30) & 3;
          shift = size; // bytes log2
        } else if ((inst & 0x3f00'0000u) == 0x3d000000u) {
          // LDR/STR (FP/SIMD, unsigned offset). Size in bits 30..31 +
          // opc<23>.
          u32 size = (inst >> 30) & 3;
          u32 opc1 = (inst >> 23) & 1;
          shift = size == 0 && opc1 ? 4 : size;
        }
        if (syma & ((uintptr_t{1} << shift) - 1)) {
          TPDE_LOG_ERR("unaligned Mach-O PAGEOFF12 relocation");
          success = false;
        }
        u32 v = u32((syma & 0xfffu) >> shift) << 10;
        blend(pc, 0xfffu << 10, v);
        break;
      }
      default:
        TPDE_LOG_ERR("unsupported Mach-O relocation type: {:#x}", reloc.type);
        success = false;
      }
    }
  }

  ::pthread_jit_write_protect_np(1);
  ::sys_icache_invalidate(mapped_addr, mapped_size);
  if (!success) {
    reset();
    return false;
  }

  // Darwin's __register_frame registers a single FDE, unlike libgcc's
  // section-wide API. Register only FDEs and deregister before unmapping.
  for (const auto &as : alloc) {
    const auto &sec = *as;
    if (sec.name != unsigned(SectionKind::EHFrame)) {
      continue;
    }
    auto *start = reinterpret_cast<u8 *>(sec.addr);
    for (size_t off = 0; off + 8 <= sec.size();) {
      u32 length, id;
      std::memcpy(&length, start + off, 4);
      std::memcpy(&id, start + off + 4, 4);
      if (!length) {
        break;
      }
      if (length < 4 || length > sec.size() - off - 4) {
        reset();
        return false;
      }
      if (id) {
        __register_frame(start + off);
        registered_fdes.push_back(start + off);
      }
      off += length + 4;
    }
  }
  for (const auto &as : alloc) {
    const auto &sec = *as;
    if ((sec.flags & SECTION_TYPE) == S_MOD_TERM_FUNC_POINTERS) {
      for (size_t off = 0; off < sec.size(); off += sizeof(void *)) {
        void (*fn)();
        std::memcpy(&fn, reinterpret_cast<void *>(sec.addr + off), sizeof(fn));
        destructors.push_back(fn);
      }
    }
  }
  for (const auto &as : alloc) {
    const auto &sec = *as;
    if ((sec.flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS) {
      for (size_t off = 0; off < sec.size(); off += sizeof(void *)) {
        void (*fn)();
        std::memcpy(&fn, reinterpret_cast<void *>(sec.addr + off), sizeof(fn));
        fn();
      }
    }
  }
  return true;
}

void *MachOMapper::get_sym_addr(SymRef sym) const {
  size_t idx = sym.id();
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
