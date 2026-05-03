; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Callee-side calling-convention edge: defining a variadic function.
;   - AAPCS = the prologue MUST spill x0..x7 / v0..v7 into a save
;            area so `va_list`'s `__gr_top`/`__vr_top` can walk
;            the register-passed varargs at runtime. ~192 bytes.
;   - darwinpcs = `va_list` is just `char*` to the variadic stack
;            overflow area; nothing to spill at function entry. The
;            save-area block is omitted entirely.
;
; Driven by `Config::VARARG_USES_REG_SAVE_AREA`: true for AAPCS,
; false for darwinpcs. Skipping the spills cuts ~192 bytes from
; the frame and 9 instructions from the prologue per variadic
; function on Darwin.

; RUN: tpde-llc --target=aarch64 %s | %objdump | FileCheck %s -check-prefixes=AAPCS
; RUN: tpde-llc --target=arm64-apple-macosx11.0 %s \
; RUN:   | llvm-objdump -d -r --triple=aarch64 --no-show-raw-insn --no-addresses - \
; RUN:   | FileCheck %s -check-prefixes=DARWIN

declare i32 @sink(i32)

define i32 @vararg_callee(ptr %fmt, ...) {
  %r = call i32 @sink(i32 0)
  ret i32 %r
}

; (AAPCS) huge frame for the save area + spills of x0..x7 + v0..v7.
; AAPCS-LABEL: <vararg_callee>:
; AAPCS:        stp x29, x30, [sp, #-0x170]!
; AAPCS-NEXT:   mov x29, sp
; AAPCS-NEXT:   stp x0, x1, [sp, #0xa0]
; AAPCS-NEXT:   stp x2, x3, [sp, #0xb0]
; AAPCS-NEXT:   stp x4, x5, [sp, #0xc0]
; AAPCS-NEXT:   stp x6, x7, [sp, #0xd0]
; AAPCS-NEXT:   stp q0, q1, [sp, #0xe0]
; AAPCS-NEXT:   stp q2, q3, [sp, #0x100]
; AAPCS-NEXT:   stp q4, q5, [sp, #0x120]
; AAPCS-NEXT:   stp q6, q7, [sp, #0x140]

; (Darwin) no save-area spills, much smaller frame.
; DARWIN-LABEL: <_vararg_callee>:
; DARWIN:        stp x29, x30, [sp, #-0xa0]!
; DARWIN-NEXT:   mov x29, sp
; DARWIN-NOT:    stp x0, x1
; DARWIN-NOT:    stp q0, q1
; DARWIN:        bl
