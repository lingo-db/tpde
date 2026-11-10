; SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
; REQUIRES: native-macho
; RUN: split-file %s %t
; RUN: tpde-lli %t/good.ll
; RUN: tpde-lli --orc %t/good.ll
; RUN: not tpde-lli %t/bad.ll 2>&1 | FileCheck %s --check-prefix=DIRECT
; RUN: not tpde-lli --orc %t/bad.ll 2>&1 | FileCheck %s --check-prefix=ORC
;
; DIRECT: JIT compilation failed
; ORC: Symbols not found: [ abs ]

;--- good.ll
target triple = "arm64-apple-macosx11.0"

declare i32 @abs(i32)
declare i32 @"\01_abs"(i32)
; This is an ordinary C name with genuine leading underscores. They must stay.
declare ptr @__error()

define i32 @main() {
  %ordinary = call i32 @abs(i32 -42)
  %explicit = call i32 @"\01_abs"(i32 -42)
  %errno = call ptr @__error()
  %a = icmp eq i32 %ordinary, 42
  %b = icmp eq i32 %explicit, 42
  %c = icmp ne ptr %errno, null
  %ab = and i1 %a, %b
  %ok = and i1 %ab, %c
  %failed = xor i1 %ok, true
  %result = zext i1 %failed to i32
  ret i32 %result
}

;--- bad.ll
target triple = "arm64-apple-macosx11.0"

; This requests the literal linker symbol "abs", not libc's "_abs".
declare i32 @"\01abs"(i32)
define i32 @main() {
  %value = call i32 @"\01abs"(i32 -42)
  %result = sub i32 %value, 42
  ret i32 %result
}
