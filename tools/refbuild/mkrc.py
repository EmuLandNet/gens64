#!/usr/bin/env python3
"""Prepare Gens.rc for GNU windres: drop the DLGINIT block (unsupported; it only pre-fills an unused
combo box), point resource paths at a copy of resource/ whose icons have their bit depth filled in
(MS rc.exe does that itself; otherwise Windows picks the 16-colour image).
Usage: mkrc.py <Gens.rc> <resource dir> <out dir>   -> <out dir>/Gens_build.rc, <out dir>/res/"""
import os, re, sys, shutil, subprocess
rc, resdir, out = sys.argv[1:4]
res = os.path.join(out, 'res')
shutil.rmtree(res, ignore_errors=True); shutil.copytree(resdir, res)
fix = os.path.join(os.path.dirname(__file__), 'fix_ico.py')
for f in sorted(os.listdir(res)):
    if f.lower().endswith('.ico'):
        subprocess.check_call([sys.executable, fix, os.path.join(res, f), os.path.join(res, f)])
s = open(rc, 'rb').read().decode('latin1').replace('\r\n', '\n').replace('\r', '\n')
s = re.sub(r'\nIDD_CONTROLLER DLGINIT\nBEGIN\n.*?\nEND\n', '\n', s, flags=re.S)
s = s.replace('"resource\\\\', '"' + res + '/')
open(os.path.join(out, 'Gens_build.rc'), 'wb').write(s.encode('latin1'))
