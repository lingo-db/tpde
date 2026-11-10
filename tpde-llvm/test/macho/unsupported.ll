; SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
; RUN: split-file %s %t
; RUN: not tpde-llc --target=arm64-apple-macosx11.0 %t/tls.ll -o /dev/null
; RUN: not tpde-llc --target=arm64-apple-macosx11.0 %t/section.ll -o /dev/null
; RUN: not tpde-llc --target=arm64-apple-macosx11.0 %t/comdat.ll -o /dev/null
; RUN: not tpde-llc --target=arm64-apple-ios %t/plain.ll -o /dev/null

;--- plain.ll
define void @f() { ret void }

;--- tls.ll
@x = thread_local global i32 0

;--- section.ll
@x = global i32 0, section "__DATA,__custom"

;--- comdat.ll
$g = comdat any
@g = global i32 0, comdat
