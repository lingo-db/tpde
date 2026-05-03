; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Verify Mach-O symbol mangling and reloc encoding for a call to an
; external function:
;   - The call site emits a `bl` with an ARM64_RELOC_BRANCH26 reloc.
;   - The reloc references the underscore-prefixed external symbol.
;   - The undefined symbol shows up in the symbol table with
;     N_UNDF | N_EXT.

; RUN: tpde-llc %s -o %t.o
; RUN: llvm-objdump -d -r --triple=aarch64 --no-show-raw-insn %t.o \
; RUN:   | FileCheck %s -check-prefix=ASM
; RUN: llvm-readobj --syms %t.o | FileCheck %s -check-prefix=SYM

target triple = "arm64-apple-macosx11.0"

declare i64 @ext_callee(i64)

define i64 @caller(i64 %x) {
  %r = call i64 @ext_callee(i64 %x)
  ret i64 %r
}

; The call instruction goes through a `bl` with an ARM64 BRANCH26
; relocation against the underscore-prefixed external. (llvm-objdump
; renders `ARM64_RELOC_BRANCH26` for the Mach-O reloc kind; the
; symbol is `_ext_callee`.)
; ASM:      <_caller>:
; ASM:        bl
; ASM:        ARM64_RELOC_BRANCH26 _ext_callee

; The undefined external lives in the symtab with N_UNDF | N_EXT.
; SYM:      Name: _ext_callee
; SYM-NEXT: Extern
; SYM-NEXT: Type: Undef
