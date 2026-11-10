; SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
; RUN: tpde-llc %s -o %t.o
; RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s
target triple = "arm64-apple-macosx11.0"
declare void @sink()
define void @caller() {
  call void @sink()
  ret void
}
; CHECK: .eh_frame contents:
; CHECK: CIE
; CHECK: FDE
; CHECK: DW_CFA_def_cfa_register: W29
