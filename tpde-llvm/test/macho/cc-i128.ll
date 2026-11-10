; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; i128 (and any 16-byte-aligned type) calling-convention edges.
;
; AAPCS rule C.7.1: NGRN is rounded up to the next even number when
; the next arg is 16-byte aligned, so the 16-byte slot lands on an
; even register pair.
;
; Apple deviation: NGRN is *not* rounded — the i128 lands wherever
; NGRN happens to be, in two consecutive registers regardless of
; parity. ("Writing ARM64 code for Apple platforms" — register
; assignment "may not need to be rounded up to the next even
; number".)
;
; Concrete observable difference, `void f(i64, i128)`:
;   AAPCS  -> i64 in x0, x1 SKIPPED, i128 in x2/x3.
;   Darwin -> i64 in x0, i128 in x1/x2.
;
; This was a real bug in `CCAssignerDarwinAArch64` (copy-pasted the
; AAPCS rounding) — fixed alongside this test.

; RUN: tpde-llc --target=aarch64 %s | %objdump | FileCheck %s -check-prefixes=AAPCS
; RUN: tpde-llc --target=arm64-apple-macosx11.0 %s \
; RUN:   | llvm-objdump -d -r --triple=aarch64 --no-show-raw-insn --no-addresses - \
; RUN:   | FileCheck %s -check-prefixes=DARWIN

declare void @sink_i64_i128(i64, i128)
declare void @sink_i128_i64(i128, i64)
declare void @sink_six_i64_i128(i64, i64, i64, i64, i64, i64, i128)
declare void @sink_seven_i64_i128(i64, i64, i64, i64, i64, i64, i64, i128)

; ---------------------------------------------------------------
; (i64, i128) — the headline AAPCS-vs-darwinpcs split.
define void @call_i64_i128() {
  call void @sink_i64_i128(i64 1, i128 2)
  ret void
}
; AAPCS-LABEL: <call_i64_i128>:
; AAPCS:        mov x0, #0x1
; AAPCS-NEXT:   mov x2, #0x2
; AAPCS-NEXT:   mov w3, #0x0
; AAPCS-NEXT:   bl
;
; DARWIN-LABEL: <_call_i64_i128>:
; DARWIN:       mov x0, #0x1
; DARWIN-NEXT:  mov x1, #0x2
; DARWIN-NEXT:  mov w2, #0x0
; DARWIN-NEXT:  bl

; ---------------------------------------------------------------
; (i128, i64) — i128 starts at NGRN=0 (already even); both ABIs
; agree: i128 = x0/x1, i64 = x2.
define void @call_i128_i64() {
  call void @sink_i128_i64(i128 1, i64 2)
  ret void
}
; AAPCS-LABEL: <call_i128_i64>:
; AAPCS:        mov x0, #0x1
; AAPCS-NEXT:   mov w1, #0x0
; AAPCS-NEXT:   mov x2, #0x2
; AAPCS-NEXT:   bl
;
; DARWIN-LABEL: <_call_i128_i64>:
; DARWIN:       mov x0, #0x1
; DARWIN-NEXT:  mov w1, #0x0
; DARWIN-NEXT:  mov x2, #0x2
; DARWIN-NEXT:  bl

; ---------------------------------------------------------------
; (6 x i64, i128) — exactly 2 regs left (x6, x7) at the i128;
; NGRN = 6 (even), no rounding needed on either ABI.
define void @call_six_i64_i128() {
  call void @sink_six_i64_i128(i64 0, i64 1, i64 2, i64 3, i64 4, i64 5, i128 6)
  ret void
}
; AAPCS-LABEL: <call_six_i64_i128>:
; AAPCS:        mov w0, #0x0
; AAPCS:        mov x6, #0x6
; AAPCS-NEXT:   mov w7, #0x0
; AAPCS-NEXT:   bl
;
; DARWIN-LABEL: <_call_six_i64_i128>:
; DARWIN:       mov w0, #0x0
; DARWIN:       mov x6, #0x6
; DARWIN-NEXT:  mov w7, #0x0
; DARWIN-NEXT:  bl

; ---------------------------------------------------------------
; (7 x i64, i128) — only 1 reg (x7) left for i128. tpde-llvm's
; `arg_allow_split_reg_stack_passing` returns false for i128, so
; the entire i128 goes to the stack on both ABIs (no register/
; stack split). The 7 i64s fill x0..x6 normally.
define void @call_seven_i64_i128() {
  call void @sink_seven_i64_i128(i64 0, i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i128 7)
  ret void
}
; AAPCS-LABEL: <call_seven_i64_i128>:
; AAPCS:        mov w0, #0x0
; AAPCS:        mov x6, #0x6
; AAPCS:        mov x7, #0x7
; AAPCS-NEXT:   str x7, [sp]
; AAPCS-NEXT:   mov w7, #0x0
; AAPCS-NEXT:   str x7, [sp, #0x8]
; AAPCS-NEXT:   bl
;
; DARWIN-LABEL: <_call_seven_i64_i128>:
; DARWIN:       mov w0, #0x0
; DARWIN:       mov x6, #0x6
; DARWIN:       mov x7, #0x7
; DARWIN-NEXT:  str x7, [sp]
; DARWIN-NEXT:  mov w7, #0x0
; DARWIN-NEXT:  str x7, [sp, #0x8]
; DARWIN-NEXT:  bl

; ---------------------------------------------------------------
; Callee-side: a function whose definition is `(i64, i128)`. The
; same NGRN-rounding rule applies: AAPCS reads i128 from x2/x3,
; Darwin from x1/x2. (Verified by where the func loads its second
; arg's halves into spill slots / pre-call regs.)
declare void @callback_i128(i128)
define void @callee_i64_i128(i64 %a, i128 %b) {
  ; Force the i128 arg to be observable in codegen by passing it on.
  call void @callback_i128(i128 %b)
  ret void
}
; AAPCS i128 was read from x2/x3 — `mov x0, x2; mov x1, x3` to
; place into x0/x1 for the callback. (NGRN order.)
; AAPCS-LABEL: <callee_i64_i128>:
; AAPCS:        mov x0, x2
; AAPCS-NEXT:   mov x1, x3
; AAPCS-NEXT:   bl
;
; Darwin i128 was read from x1/x2 — `mov x2, x1; mov x0, x2; ...`
; (different copy chain because operands aren't in canonical
; positions). The key signal: x3 is NOT in the move chain on
; Darwin since the i128 high half came from x2, not x3.
; DARWIN-LABEL: <_callee_i64_i128>:
; DARWIN:       mov
; DARWIN-NOT:   mov x{{[01]}}, x3
; DARWIN:       bl
