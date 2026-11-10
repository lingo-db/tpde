// SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/AssemblerMachO.hpp"
#include "tpde/MachOMapper.hpp"
#include <cstdio>
#include <cstring>

using namespace tpde;
using namespace tpde::macho;

static unsigned structor_count;
static void initialize() { ++structor_count; }
static void terminate() { --structor_count; }

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::fprintf(stderr, "FAIL: %s\n", #expr);                               \
      return false;                                                            \
    }                                                                          \
  } while (0)

bool test_macho_assembler() {
  AssemblerMachOA64 a;
  const auto data = a.get_default_section(SectionKind::Data);
  const auto value = a.sym_predef_data("value", Assembler::SymBinding::LOCAL);
  CHECK(value.valid());
  a.sym_def_predef_zero(data, value, 16, 8);
  a.set_section_retain(data);
  a.reloc_abs(data, value, 8, 7);
  a.get_section(data).addr = 1234;
  auto object = a.build_object_file();
  CHECK(object == a.build_object_file());
  CHECK(a.get_section(data).addr == 1234);
  CHECK(a.get_section(data).data[8] ==
        0); // Inline addends belong to the output.
  const auto *section = reinterpret_cast<const section_64 *>(
      object.data() + sizeof(mach_header_64) + sizeof(segment_command_64));
  CHECK(section->flags & S_ATTR_NO_DEAD_STRIP);
  u64 addend;
  std::memcpy(&addend, object.data() + section->offset + 8, 8);
  CHECK(addend == 7);

  MachOMapper mapper;
  const auto no_symbols = [](std::string_view) -> void * { return nullptr; };
  CHECK(mapper.map(a, no_symbols));
  auto *mapped = static_cast<u64 *>(mapper.get_sym_addr(value));
  CHECK(mapped && mapped[1] == uintptr_t(mapped) + 7);
  const auto mapped_address = a.get_section(data).addr;
  CHECK(object == a.build_object_file());
  CHECK(a.get_section(data).addr == mapped_address);
  *mapped = 42; // Data must remain writable while JIT execution is enabled.
  CHECK(*mapped == 42);
  CHECK(mapper.map(a, no_symbols)); // Remapping releases the old allocation.
  CHECK(*static_cast<u64 *>(mapper.get_sym_addr(value)) == 0);
  mapper.reset();
  a.reset();
  CHECK(a.get_default_section(SectionKind::Text).valid());
  CHECK(a.sym_predef_data("again", Assembler::SymBinding::LOCAL).valid());

  // Stable symbol IDs need not follow the on-disk local/defined/undefined
  // order.
  AssemblerMachOA64 mixed;
  const auto mixed_data = mixed.get_default_section(SectionKind::Data);
  (void)mixed.sym_add_undef("external", Assembler::SymBinding::GLOBAL);
  const auto global =
      mixed.sym_predef_data("global", Assembler::SymBinding::GLOBAL);
  const auto local =
      mixed.sym_predef_data("local", Assembler::SymBinding::LOCAL);
  const auto weak = mixed.sym_predef_data("weak", Assembler::SymBinding::WEAK);
  (void)mixed.sym_add_undef("weak_external", Assembler::SymBinding::WEAK);
  const auto alias =
      mixed.sym_predef_data("alias", Assembler::SymBinding::GLOBAL);
  for (auto sym : {global, local, weak}) {
    mixed.sym_def_predef_zero(mixed_data, sym, 8, 8);
  }
  mixed.sym_copy(alias, local);
  mixed.reloc_abs(mixed_data, alias, 0, 3);
  auto mixed_object = mixed.build_object_file();
  const auto *header =
      reinterpret_cast<const mach_header_64 *>(mixed_object.data());
  const symtab_command *symtab = nullptr;
  const dysymtab_command *groups = nullptr;
  size_t offset = sizeof(mach_header_64);
  for (u32 i = 0; i < header->ncmds; ++i) {
    const auto *cmd =
        reinterpret_cast<const u32 *>(mixed_object.data() + offset);
    if (cmd[0] == LC_SYMTAB) {
      symtab = reinterpret_cast<const symtab_command *>(cmd);
    }
    if (cmd[0] == LC_DYSYMTAB) {
      groups = reinterpret_cast<const dysymtab_command *>(cmd);
    }
    offset += cmd[1];
  }
  CHECK(symtab && groups);
  CHECK(groups->nlocalsym == 1 && groups->nextdefsym == 3 &&
        groups->nundefsym == 2);
  const auto *syms =
      reinterpret_cast<const nlist_64 *>(mixed_object.data() + symtab->symoff);
  CHECK(std::strcmp(reinterpret_cast<const char *>(
                        mixed_object.data() + symtab->stroff + syms[0].n_strx),
                    "_local") == 0);
  CHECK(syms[2].n_desc & N_WEAK_DEF);
  CHECK(syms[3].n_value ==
        syms[0].n_value); // Alias still names the local definition.
  CHECK(syms[5].n_desc & N_WEAK_REF);
  CHECK(mapper.map(mixed,
                   no_symbols)); // Unreferenced externals need no resolver.
  CHECK(mapper.get_sym_addr(alias) == mapper.get_sym_addr(local));
  CHECK(*static_cast<u64 *>(mapper.get_sym_addr(global)) ==
        uintptr_t(mapper.get_sym_addr(alias)) + 3);
  CHECK(mixed_object == mixed.build_object_file());
  mapper.reset();

  // Object alignment must retain the full exponent, even above 32 KiB.
  mixed.get_section(mixed_data).align = 65536;
  mixed_object = mixed.build_object_file();
  const auto *aligned = reinterpret_cast<const section_64 *>(
      mixed_object.data() + sizeof(mach_header_64) +
      sizeof(segment_command_64));
  CHECK(aligned->align == 16 && aligned->offset % 65536 == 0);

  // Virtual alignment must not allocate padding in the object file.
  AssemblerMachOA64 virtual_data;
  const auto bss = virtual_data.get_default_section(SectionKind::BSS);
  const auto zero =
      virtual_data.sym_predef_data("zero", Assembler::SymBinding::LOCAL);
  virtual_data.sym_def_predef_zero(bss, zero, 8, 1u << 20);
  CHECK(virtual_data.build_object_file().size() < (1u << 20));

  // The LDR Q immediate is scaled by 16; integer and SIMD opcodes overlap.
  AssemblerMachOA64 simd;
  const auto text = simd.get_default_section(SectionKind::Text);
  const auto rodata = simd.get_default_section(SectionKind::ReadOnly);
  simd.get_section(text).write<u32>(0x3dc00000); // ldr q0, [x0]
  const auto load = simd.sym_predef_func("load", Assembler::SymBinding::GLOBAL);
  simd.sym_def(load, text, 0, 4);
  const auto constant =
      simd.sym_predef_data("constant", Assembler::SymBinding::LOCAL);
  simd.sym_def_predef_zero(rodata, constant, 32, 16);
  simd.reloc_sec(text, constant, ARM64_RELOC_PAGEOFF12, 0, 16);
  CHECK(mapper.map(simd, no_symbols));
  const auto inst = *static_cast<u32 *>(mapper.get_sym_addr(load));
  const auto addr = uintptr_t(mapper.get_sym_addr(constant)) + 16;
  CHECK(((inst >> 10) & 4095) == ((addr & 4095) >> 4));
  mapper.reset();

  // Even a symbol available in the host process must be rejected when the
  // caller declines it. Failed mapping leaves an empty, reusable mapper.
  AssemblerMachOA64 external;
  const auto refs = external.get_default_section(SectionKind::Data);
  external.get_section(refs).write<u64>(0);
  const auto missing =
      external.sym_add_undef("puts", Assembler::SymBinding::GLOBAL);
  external.reloc_abs(refs, missing, 0, 0);
  CHECK(!mapper.map(external, no_symbols));
  CHECK(mapper.get_mapped_range().first == nullptr);
  static u64 host_value;
  CHECK(mapper.map(external,
                   [](std::string_view) -> void * { return &host_value; }));
  mapper.reset();

  // A declined weak reference stays null even if the host exports the name.
  AssemblerMachOA64 weak_refs;
  const auto weak_data = weak_refs.get_default_section(SectionKind::Data);
  const auto weak_slot =
      weak_refs.sym_predef_data("slot", Assembler::SymBinding::LOCAL);
  weak_refs.sym_def_predef_zero(weak_data, weak_slot, 8, 8);
  const auto weak_puts =
      weak_refs.sym_add_undef("puts", Assembler::SymBinding::WEAK);
  weak_refs.reloc_abs(weak_data, weak_puts, 0, 0);
  CHECK(mapper.map(weak_refs, no_symbols));
  CHECK(*static_cast<void **>(mapper.get_sym_addr(weak_slot)) == nullptr);
  mapper.reset();

  // Explicit assembler names are passed unchanged to custom resolvers.
  AssemblerMachOA64 explicit_names;
  const auto explicit_data =
      explicit_names.get_default_section(SectionKind::Data);
  explicit_names.get_section(explicit_data).write<u64>(0);
  const auto explicit_sym =
      explicit_names.sym_add_undef("\1_custom", Assembler::SymBinding::GLOBAL);
  explicit_names.reloc_abs(explicit_data, explicit_sym, 0, 0);
  bool saw_explicit_name = false;
  CHECK(mapper.map(explicit_names, [&](std::string_view name) -> void * {
    saw_explicit_name = name == "\1_custom";
    return saw_explicit_name ? &host_value : nullptr;
  }));
  CHECK(saw_explicit_name);
  mapper.reset();

  // Each out-of-range branch must target symbol + addend, not thunk + addend.
  AssemblerMachOA64 branches;
  const auto code = branches.get_default_section(SectionKind::Text);
  const auto entry =
      branches.sym_predef_func("entry", Assembler::SymBinding::GLOBAL);
  branches.sym_def(entry, code, 0, 8);
  const auto target =
      branches.sym_add_undef("far_target", Assembler::SymBinding::GLOBAL);
  for (u32 off : {0u, 4u}) {
    branches.get_section(code).write<u32>(0x14000000); // b
    branches.reloc_sec(code, target, ARM64_RELOC_BRANCH26, off, off + 4);
  }
  CHECK(mapper.map(branches, [](std::string_view) -> void * {
    return reinterpret_cast<void *>(0x1000);
  }));
  const auto pc = uintptr_t(mapper.get_sym_addr(entry));
  for (u32 off : {0u, 4u}) {
    const u32 instruction = *reinterpret_cast<u32 *>(pc + off);
    const i32 displacement = i32(instruction << 6) >> 4;
    const auto thunk = pc + off + displacement;
    CHECK(*reinterpret_cast<uintptr_t *>(thunk + 8) == 0x1000 + off + 4);
  }
  mapper.reset();

  // Initializers run after relocation; terminators run before unmapping.
  AssemblerMachOA64 structors;
  for (bool init : {true, false}) {
    const auto sec = structors.create_structor_section(init);
    structors.get_section(sec).write<u64>(0);
    const auto fn = structors.sym_add_undef(init ? "init" : "term",
                                            Assembler::SymBinding::GLOBAL);
    structors.reloc_abs(sec, fn, 0, 0);
  }
  CHECK(mapper.map(structors, [](std::string_view name) -> void * {
    return reinterpret_cast<void *>(name == "init" ? initialize : terminate);
  }));
  CHECK(structor_count == 1);
  mapper.reset();
  CHECK(structor_count == 0);

  // Reject unsupported alignment rather than silently weakening it.
  simd.get_section(rodata).align = 1u << 24;
  CHECK(!mapper.map(simd, no_symbols));
  return true;
}
