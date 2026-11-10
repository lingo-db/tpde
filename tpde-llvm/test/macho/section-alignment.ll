; SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
; RUN: tpde-llc %s -o %t.o
; RUN: llvm-readobj --sections %t.o | FileCheck %s

target triple = "arm64-apple-macosx11.0"
@aligned = global i64 42, align 65536

; CHECK: Name: __data
; CHECK: Alignment: 16
