# SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
import pathlib, subprocess, sys
cxx, obj, base = sys.argv[1:]
src = pathlib.Path(base + '.cpp')
src.write_text(r'''#include <cstdio>
extern "C" unsigned long bridge(unsigned long);
extern "C" unsigned long thrower() { throw 42; }
int main() {
  register unsigned long sentinel asm("x19") = 0x123456789abcdef0ul;
  asm volatile("" : "+r"(sentinel));
  try { bridge(55); } catch (int x) {
    asm volatile("" : "+r"(sentinel));
    return x == 42 && sentinel == 0x123456789abcdef0ul ? 0 : 1;
  }
  return 2;
}
''')
subprocess.run([cxx, '-O1', '-fexceptions', str(src), obj, '-o', base + '.exe'], check=True)
subprocess.run([base + '.exe'], check=True)
