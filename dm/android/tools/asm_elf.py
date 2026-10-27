# Mach-O AArch64 assembly (the host's .S files) → ELF for the Android build: C symbols lose their leading underscore, sym@PAGE / sym@PAGEOFF become sym / :lo12:sym,
# .cstring becomes a mergeable string section, and global symbols in code get a %function type (data symbols stay untyped).
# Usage: asm_elf.py <in.S> <out.S>; the output is written only when it changes (keeps the build quiet)
import re, sys
src, dst = sys.argv[1], sys.argv[2]
out, code = [], True
for line in open(src).read().split('\n'):
    line = re.sub(r'(?<![\w.$])_([A-Za-z]\w*)@PAGEOFF\b', r':lo12:\1', line)
    line = re.sub(r'(?<![\w.$])_([A-Za-z]\w*)@PAGE\b', r'\1', line)
    line = re.sub(r'(?<![\w.$:])_([A-Za-z]\w*)', r'\1', line)
    d = line.split('//')[0].strip()
    if d in ('.text',) or d.startswith('.section __TEXT,__text'): code = True
    elif d in ('.data', '.const', '.cstring') or d.startswith('.section'): code = False
    if d == '.cstring': line = line.replace('.cstring', '.section .rodata.str1.1,"aMS",@progbits,1')
    elif d == '.const': line = line.replace('.const', '.section .rodata')
    out.append(line)
    m = re.match(r'^([ \t]*)\.globl[ \t]+(\w+)', line)
    if m and code: out.append(f'{m.group(1)}.type {m.group(2)}, %function')
s = '\n'.join(out)
assert '@PAGE' not in s and '@GOT' not in s, 'Mach-O relocation left'
try: old = open(dst).read()
except OSError: old = None
if old != s: open(dst, 'w').write(s)
