; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Caller-side calling-convention edge: a call to a variadic function.
; Where AAPCS and darwinpcs disagree:
;   - AAPCS = variadic args fill registers first (x0..x7, v0..v7),
;            only the overflow goes on the stack. The same as
;            named args.
;   - darwinpcs = all variadic args go on the stack at natural
;            alignment, regardless of register availability.
;
; Today we implement a simplified "all args on stack when vararg"
; rule for darwinpcs (everything to stack, including the named
; portion). The proper named/variadic split is rough plan §M3
; follow-up; until then this test pins the *current* behavior so
; refactors don't silently regress.

; RUN: tpde-llc --target=aarch64 %s | %objdump | FileCheck %s -check-prefixes=AAPCS
; RUN: tpde-llc --target=arm64-apple-macosx11.0 %s \
; RUN:   | llvm-objdump -d -r --triple=aarch64 --no-show-raw-insn --no-addresses - \
; RUN:   | FileCheck %s -check-prefixes=DARWIN

declare i32 @fn_var_arg(ptr, ...)
declare i32 @fn_normal(ptr, i32)

; --- Variadic call: AAPCS keeps args in regs, darwinpcs goes to stack.
define i32 @call_vararg_one_int(ptr %fmt, i32 %x) {
  %r = call i32 (ptr, ...) @fn_var_arg(ptr %fmt, i32 %x)
  ret i32 %r
}
; AAPCS-LABEL: <call_vararg_one_int>:
; AAPCS:        stp x29, x30, [sp, #-0x{{[0-9a-f]+}}]!
; AAPCS-NEXT:   mov x29, sp
; AAPCS-NOT:    str
; AAPCS:        bl
; AAPCS-NEXT:    R_AARCH64_CALL26 fn_var_arg
;
; DARWIN-LABEL: <_call_vararg_one_int>:
; DARWIN:       stp x29, x30, [sp, #-0x{{[0-9a-f]+}}]!
; DARWIN-NEXT:  mov x29, sp
; DARWIN-NEXT:  sub sp, sp, #0x10
; DARWIN-NEXT:  str x0, [sp]
; DARWIN-NEXT:  str w1, [sp, #0x8]
; DARWIN-NEXT:  bl
; DARWIN-NEXT:    ARM64_RELOC_BRANCH26 _fn_var_arg

; --- Non-variadic call with same arg shape: BOTH keep args in regs.
define i32 @call_normal_one_int(ptr %fmt, i32 %x) {
  %r = call i32 @fn_normal(ptr %fmt, i32 %x)
  ret i32 %r
}
; AAPCS-LABEL: <call_normal_one_int>:
; AAPCS:        stp x29, x30, [sp, #-0x{{[0-9a-f]+}}]!
; AAPCS-NOT:    str x0, [sp]
; AAPCS:        bl
; AAPCS-NEXT:    R_AARCH64_CALL26 fn_normal
;
; DARWIN-LABEL: <_call_normal_one_int>:
; DARWIN:       stp x29, x30, [sp, #-0x{{[0-9a-f]+}}]!
; DARWIN-NOT:   str x0, [sp]
; DARWIN:       bl
; DARWIN-NEXT:    ARM64_RELOC_BRANCH26 _fn_normal
