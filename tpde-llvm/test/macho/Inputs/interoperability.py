# SPDX-FileCopyrightText: 2026 Contributors to TPDE <https://tpde.org>
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
from pathlib import Path
import subprocess

import sys
clang, llc, lli, directory = sys.argv[1:]
root = Path(directory).resolve()
root.mkdir(parents=True, exist_ok=True)
cases = {
 'narrow_stack': (
  'int f(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j){return j;}',
  'extern int f(int,int,int,int,int,int,int,int,int,int); int main(){return f(1,2,3,4,5,6,7,8,9,42)==42?0:1;}'),
 'vararg_caller': (
  'extern int sink(int,...); int f(int x){return sink(100,x);}',
  '#include <stdarg.h>\nextern int f(int); int sink(int x,...){va_list ap; va_start(ap,x); int y=va_arg(ap,int); va_end(ap); return x*1000+y;} int main(){return f(42)==100042?0:1;}'),
 'vararg_callee': (
  '#include <stdarg.h>\nint f(int x,...){va_list ap; va_start(ap,x); int y=va_arg(ap,int); va_end(ap); return x+y;}',
  'extern int f(int,...); int main(){return f(100,42)==142?0:1;}'),
 'absolute_addend': (
  'char text[]="abcdef"; char *p=text+3; int f(){return *p;}',
  'extern int f(void); int main(){return f()==100?0:1;}'),
 'ctor': (
  'volatile int x; __attribute__((constructor)) void init(){x=42;} int f(){return x;}',
  'extern int f(void); int main(){return f()==42?0:1;}'),
 'narrow_mixed_stack': (
  'long f(long a,long b,long c,long d,long e,long f,long g,long h,char i,short j,int k){return i+j+k;}',
  'extern long f(long,long,long,long,long,long,long,long,char,short,int); int main(){return f(0,0,0,0,0,0,0,0,-3,257,42)==296?0:1;}'),
 'vararg_canaries': (
  '#include <stdarg.h>\nint f(int x,...){struct {va_list ap; volatile unsigned long guard;} a,b; a.guard=123456; b.guard=654321; va_start(a.ap,x); va_copy(b.ap,a.ap); int y=va_arg(a.ap,int); int z=va_arg(b.ap,int); va_end(a.ap); va_end(b.ap); return a.guard==123456 && b.guard==654321 && y==z ? x+y : -1;}',
  'extern int f(int,...); int main(){return f(100,42)==142?0:1;}'),
 'vararg_named_stack': (
  '#include <stdarg.h>\nint f(int a,int b,int c,int d,int e,int f,int g,int h,int i,...){va_list ap; va_start(ap,i); int j=va_arg(ap,int); double k=va_arg(ap,double); va_end(ap); return i+j+(int)k;}',
  'extern int f(int,int,int,int,int,int,int,int,int,...); int main(){return f(0,0,0,0,0,0,0,0,10,12,20.0)==42?0:1;}'),
 'vararg_i128': (
  '#include <stdarg.h>\nlong f(int x,...){va_list ap; va_start(ap,x); long a=va_arg(ap,long); __int128 b=va_arg(ap,__int128); va_end(ap); return a+(long)(b>>64)+(long)b;}',
  'extern long f(int,...); int main(){return f(0,7L,((__int128)15<<64)+20)==42?0:1;}'),
 'i128_argument': (
  'unsigned long f(long x, __uint128_t y){return (unsigned long)y ^ (unsigned long)(y>>64) ^ x;}',
  'extern unsigned long f(long,__uint128_t); int main(){__uint128_t x=((__uint128_t)0x123456789abcdef0ULL<<64)|0xfedcba9876543210ULL; return f(42,x)==(0x123456789abcdef0ULL^0xfedcba9876543210ULL^42)?0:1;}'),
 'explicit_symbol': (
  'extern int abs(int) __asm__("_abs"); int f(int x){return abs(x);}',
  'extern int f(int); int main(){return f(-42)==42?0:1;}'),
}
# Force a named stack area larger than an ADD-immediate can encode.
parameters = ','.join(f'long a{i}' for i in range(530))
arguments = ','.join('0L' for _ in range(530))
cases['large_vararg_frame'] = (
 '#include <stdarg.h>\nlong f(' + parameters + ',...){va_list ap; va_start(ap,a529); long x=va_arg(ap,long); va_end(ap); return x;}',
 'extern long f(' + ','.join('long' for _ in range(530)) + ',...); int main(){return f(' + arguments + ',42L)==42?0:1;}')
def run(case, args):
    command = [str(arg) for arg in args]
    try:
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=20)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"{case}: {command}: {error}\n{error.stdout}\n{error.stderr}") from error

for name, (source, host) in cases.items():
    p = root / name
    host_path = root / (name + '_host.c')
    p.with_suffix('.c').write_text(source)
    host_path.write_text(host)
    run(name, [clang, '-O1', '-S', '-emit-llvm', p.with_suffix('.c'), '-o', p.with_suffix('.ll')])
    run(name, [clang, '-O1', p.with_suffix('.c'), host_path, '-o', root / (name + '_control')])
    run(name, [root / (name + '_control')])
    run(name, [llc, '--regular-exit', '--compile-twice=1', p.with_suffix('.ll'), '-o', p.with_suffix('.o')])
    run(name, [clang, '-O1', p.with_suffix('.o'), host_path, '-o', root / (name + '_tpde')])
    run(name, [root / (name + '_tpde')])

ir_cases={
 'mutable_global': '@counter = hidden global i32 0\ndefine i32 @main() { store i32 42, ptr @counter\n ret i32 0\n}\n',
 'external_got': '@environ = external global ptr\ndefine i32 @main() { %p = load ptr, ptr @environ\n %c = icmp eq ptr %p, null\n %r = zext i1 %c to i32\n ret i32 %r\n}\n',
 'unused_declaration': 'declare void @tpde_missing_unreferenced_symbol()\ndefine i32 @main() { ret i32 0 }\n',
 'zero_sized_global': '@empty = hidden global [0 x i8] zeroinitializer\ndefine i32 @main() { %c = icmp eq ptr @empty, null\n %r = zext i1 %c to i32\n ret i32 %r\n}\n',
 'explicit_jit_symbol': 'declare i32 @"\\01_abs"(i32)\ndefine i32 @main() { %x = call i32 @"\\01_abs"(i32 -42)\n %r = sub i32 %x, 42\n ret i32 %r\n}\n',
 'weak_reference': '@tpde_missing_weak = extern_weak global i8\ndefine i32 @main() { %c = icmp ne ptr @tpde_missing_weak, null\n %r = zext i1 %c to i32\n ret i32 %r\n}\n',
}
for name, ir in ir_cases.items():
    p = root / (name + '.ll')
    p.write_text('target triple = "arm64-apple-macosx11.0"\n' + ir)
    run(name, [lli, p])
