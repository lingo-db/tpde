; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; The `tpde-llc` AOT-linkable counterpart to `lli-exec.ll`: emit a
; Mach-O `.o`, link it through `clang` (which invokes Apple `ld`),
; run, and verify the exit code. This exercises:
;   - The Mach-O object writer end-to-end (header, segment, sections,
;     symbol table sort, relocations, build-version command).
;   - Compact-unwind records being acceptable to `ld` (the linker
;     consumes our `__LD,__compact_unwind` and rolls it into the
;     final `__TEXT,__unwind_info`).

; RUN: tpde-llc %s -o %t.o
; RUN: echo 'extern long add(long, long); int main(){ return add(40,2); }' > %t.c
; RUN: clang -arch arm64 -mmacosx-version-min=11.0 %t.c %t.o -o %t.exe
; RUN: %t.exe; test $? -eq 42

target triple = "arm64-apple-macosx11.0"

define i64 @add(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}
