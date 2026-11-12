#!/usr/bin/env python3
"""Relink an existing reference build with a different memory layout (objects in reverse order plus a
padding object first), to check that per-frame state hashes do not depend on host addresses.
Usage: link_layout_variant.py <cmake build dir> <output.exe>"""
import os, shlex, subprocess, sys, tempfile
bdir, out = sys.argv[1], os.path.abspath(sys.argv[2])
cmd = subprocess.run(['ninja', '-C', bdir, '-t', 'commands', 'Gens.exe'], capture_output=True, text=True,
                     check=True).stdout.strip().splitlines()[-1]
cmd = cmd.removeprefix(': && ').removesuffix(' && :')
args = shlex.split(cmd)
objs = [a for a in args if a.endswith(('.obj', '.o')) and not a.startswith('-')]
first = args.index(objs[0])
rest = [a for a in args if a not in objs]
tmp = tempfile.mkdtemp()
pad_c, pad_o = os.path.join(tmp, 'pad.c'), os.path.join(tmp, 'pad.o')
open(pad_c, 'w').write('char gens64_layout_pad[123457]; int gens64_layout_pad2 = 5;\n')
subprocess.run(['clang', '--target=i686-w64-mingw32', '-fno-common', '-c', pad_c, '-o', pad_o], check=True)
new = rest[:first] + [pad_o] + objs[::-1] + rest[first:]
new[new.index('-o') + 1] = out
r = subprocess.run(new, cwd=bdir, capture_output=True, text=True)
sys.stderr.write('\n'.join(l for l in r.stderr.splitlines() if 'drectve' not in l))
sys.exit(r.returncode)
