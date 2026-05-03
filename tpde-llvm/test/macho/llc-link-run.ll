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
; RUN: echo 'extern long add(long, long); extern unsigned long extract(long, __uint128_t); int main(){ if (add(40,2)!=42) return 1; __uint128_t b=((__uint128_t)0xDEADBEEFCAFEBABEull)|((__uint128_t)0x1122334455667788ull<<64); return extract(7, b)==0xDEADBEEFCAFEBABEull?0:2; }' > %t.c
; RUN: clang -arch arm64 -mmacosx-version-min=11.0 %t.c %t.o -o %t.exe
; RUN: %t.exe

target triple = "arm64-apple-macosx11.0"

define i64 @add(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

; Cross-ABI round-trip: clang places `b`'s low half in x1 per
; darwinpcs (no NGRN round-up after `i64 a` in x0). The JIT'd
; function must read from x1 to recover `b_lo` correctly. If our
; CC assigner mistakenly round-up'd NGRN (AAPCS-style), `b_lo`
; would be read from x2 and `extract` would return garbage.
define i64 @extract(i64 %a, i128 %b) {
  %lo = trunc i128 %b to i64
  ret i64 %lo
}
