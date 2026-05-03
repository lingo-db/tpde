; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; End-to-end JIT-and-execute through `tpde-lli` on Apple Silicon.
; Exercises the full Mach-O JIT path: AssemblerMachOA64 produces an
; in-memory image, MachOMapper does MAP_JIT + relocation + I-cache
; invalidation, libunwind dynamic registration goes through, and the
; JIT'd `main` returns to the host harness. Output is the exit code
; — `lli` propagates `main`'s return, and FileCheck verifies it.

; RUN: tpde-lli %s; test $? -eq 42

target triple = "arm64-apple-macosx11.0"

define i64 @add(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

define i32 @main() {
  %r = call i64 @add(i64 40, i64 2)
  %r32 = trunc i64 %r to i32
  ret i32 %r32
}
