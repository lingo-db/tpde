; SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
; RUN: tpde-llc %s -o %t.o
; RUN: llvm-readobj --relocations %t.o | FileCheck %s
;
; Unwind references must identify each function atom, preferring the external
; alias at that address over the local implementation symbol.
; CHECK-DAG: ARM64_RELOC_UNSIGNED{{.*}} _first_alias
; CHECK-DAG: ARM64_RELOC_UNSIGNED{{.*}} _second_alias

target triple = "arm64-apple-macosx11.0"
@first_alias = alias i64 (i64), ptr @first
@second_alias = alias i64 (i64), ptr @second

define internal i64 @first(i64 %x) uwtable {
  ret i64 %x
}

define internal i64 @second(i64 %x) uwtable {
  %y = add i64 %x, 1
  ret i64 %y
}
