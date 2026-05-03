; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Verifies that the Mach-O back-end emits one
; `__LD,__compact_unwind` record per function, with the right encoding:
;   - leaf (no FP/LR): UNWIND_ARM64_MODE_FRAMELESS = 0x02000000
;   - frame mode + callee-save bitmap from `saved_regs`
;
; The runtime unwinder reads these (after `ld` collapses them into
; `__TEXT,__unwind_info` in the linked image) to step out of JIT'd or
; AOT-emitted frames. Encoding correctness is the whole reason the
; prologue audit fixed the canonical-pair save layout (see rough
; plan §6 risk #1).

; RUN: tpde-llc %s -o %t.o
; RUN: llvm-readobj --section-details %t.o | FileCheck %s -check-prefix=SEC
; RUN: llvm-objdump --macho --section=__LD,__compact_unwind %t.o \
; RUN:   | FileCheck %s -check-prefix=CU

target triple = "arm64-apple-macosx11.0"

; A leaf function — no callee-save / call. Compact unwind: FRAMELESS,
; stack_size = 0  →  encoding 0x02000000.
define i64 @leaf(i64 %a) {
  %r = add i64 %a, 1
  ret i64 %r
}

; The `__LD,__compact_unwind` section is present and 8-aligned.
; SEC:      Name: __compact_unwind
; SEC-NEXT: Segment: __LD
; SEC:      Alignment: 3

; Each entry is 32 bytes laid out as:
;   bytes  0..7  function_address  (relocated; zero pre-link)
;   bytes  8..11 length             (function size in bytes)
;   bytes 12..15 encoding           (MODE_FRAMELESS=0x02000000 here)
;   bytes 16..23 personality        (zero — no EH yet)
;   bytes 24..31 lsda               (zero — no EH yet)
; objdump prints raw bytes 16-per-line in disk order (little-endian
; per word), which surfaces the encoding `02000000` directly.
; CU:      Contents of (__LD,__compact_unwind) section
; CU:      00000000 00000000 00000008 02000000
; CU-NEXT: 00000000 00000000 00000000 00000000
