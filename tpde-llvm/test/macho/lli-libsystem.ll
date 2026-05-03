; SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; Confirms the JIT mapper's external-symbol resolution works for
; libSystem functions. The mapper falls back to `dlsym(RTLD_DEFAULT,
; ...)` when no user-supplied resolver matches — which means standard
; libc functions (here `puts`) just work from JIT'd code.

; RUN: tpde-lli %s | FileCheck %s

target triple = "arm64-apple-macosx11.0"

@.str = private constant [13 x i8] c"hello macho\0A\00"

declare i32 @puts(ptr) nounwind

define i32 @main() {
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}

; CHECK: hello macho
