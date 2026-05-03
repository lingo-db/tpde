; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Smallest possible Mach-O sanity check: a leaf function compiled
; through the Apple Silicon back-end. Verifies:
;   - File magic + cputype reach Mach-O 64 / arm64.
;   - Symbol names get the leading `_` prefix at write time.
;   - The function lands in `__TEXT,__text` with the canonical
;     PURE_INSTRUCTIONS attributes.

; RUN: tpde-llc %s -o %t.o
; RUN: llvm-readobj --file-headers %t.o | FileCheck %s -check-prefix=HDR
; RUN: llvm-readobj --syms %t.o | FileCheck %s -check-prefix=SYM
; RUN: llvm-readobj --section-details %t.o | FileCheck %s -check-prefix=SEG

target triple = "arm64-apple-macosx11.0"

define i64 @add(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

; HDR:      Format: Mach-O arm64
; HDR:      Magic: Magic64
; HDR:      CpuType: Arm64
; HDR:      FileType: Relocatable

; The symbol's name in the produced object includes the Mach-O
; underscore prefix even though the LLVM IR uses bare `add`.
; SYM:      Name: _add
; SYM:      Type: Section (0xE)

; The function lands in `__TEXT,__text` with PURE_INSTRUCTIONS attrs.
; SEG:        Name: __text
; SEG-NEXT:   Segment: __TEXT
; SEG:        Attributes [
; SEG-NEXT:     PureInstructions
; SEG-NEXT:     SomeInstructions
; SEG-NEXT:   ]
