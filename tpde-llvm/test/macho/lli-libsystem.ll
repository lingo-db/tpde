; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Confirms tpde-lli resolves libSystem functions through its process-symbol
; resolver. The mapper itself does not perform implicit process lookup.
;
; REQUIRES: native-macho
; RUN: tpde-lli %s | FileCheck %s

target triple = "arm64-apple-macosx11.0"

@.str = private constant [13 x i8] c"hello macho\0A\00"

declare i32 @puts(ptr) nounwind

define i32 @main() {
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}

; CHECK: hello macho
