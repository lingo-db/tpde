# SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
#
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
import lit.formats

from lit.llvm import llvm_config

config.name = 'TPDE-LLVM'
config.test_format = lit.formats.ShTest(True)

config.suffixes = ['.ll', '.cpp', '.test']

config.test_source_root = os.path.dirname(__file__)
config.environment["FILECHECK_OPTS"] = "--enable-var-scope --dump-input-filter=all --allow-unused-prefixes=false"

# Tweak the PATH to include the tools dir and TPDE binaries.
llvm_config.with_environment('PATH', config.llvm_tools_dir, append_path=True)
llvm_config.with_environment('PATH', config.tpde_llvm_bin_dir, append_path=True)
# Abort on ASan errors so that also tests running with "not" fail.
llvm_config.with_environment('ASAN_OPTIONS', "abort_on_error=1", append_path=True)
config.substitutions.append(('tpde-llc', 'tpde-llc --regular-exit'))
config.substitutions.append(('%tpde-plugin', config.tpde_llvm_bin_dir + "/tpde-plugin" + config.llvm_plugin_ext))
config.substitutions.append(('%objdump', 'llvm-objdump -d -r --no-show-raw-insn --symbolize-operands --no-addresses --x86-asm-syntax=intel -'))

# `%objdump-darwin`: same as `%objdump` but tuned for Mach-O ARM64
# output so that test CHECK lines authored for the ELF ARM64 path
# also pass on the Apple Silicon back-end:
#   - `--triple=aarch64` forces the standard (non-Apple) NEON syntax
#     so vector ops print as `add v0.8b, v1.8b, v2.8b` not `add.8b ...`.
#   - The `sed` pipe strips the leading Mach-O `_` from `<_foo>:`
#     headers and `_foo` operand references; the underscore is purely
#     a Mach-O linkage convention and the existing CHECK lines were
#     written against the ELF (no-prefix) form. Multiple no-backref
#     subs because LIT's substitution engine reinterprets `\1` etc.
config.substitutions.append((
    '%objdump-darwin',
    "llvm-objdump -d -r --triple=aarch64 --no-show-raw-insn "
    "--symbolize-operands --no-addresses - "
    "| sed -e 's/<_/</g' -e 's/ _/ /g' -e 's/,_/,/g' "
    "-e 's/(_/(/g' -e 's/\\[_/[/g'"
))

config.available_features.add(f'llvm{config.llvm_version}')
config.available_features.add(f'os-{config.system_name.lower()}')
if config.enable_llvm_plugin:
    config.available_features.add("tpde-plugin")
# Mach-O / Apple Silicon back-end: only present on `__APPLE__` builds
# of `tpde_llvm_impl`. Tests under `test/macho/` gate on this so they
# UNSUPPORTED-skip on Linux.
if config.system_name == 'Darwin':
    config.available_features.add('tpde-llvm-arm64-darwin')

# TODO(ts): arch config
