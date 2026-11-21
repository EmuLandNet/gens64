#!/usr/bin/env python3
"""Prepare Gens.rc for GNU windres: drop the DLGINIT block (unsupported; it only pre-fills an unused
combo box), point resource paths at a copy of resource/ whose icons have their bit depth filled in
(MS rc.exe does that itself; otherwise Windows picks the 16-colour image).
Usage: mkrc.py <Gens.rc> <resource dir> <out dir> [gens64_version.h]
   -> <out dir>/Gens_build.rc, <out dir>/res/
With the version header, the About box shows the gens64 revision and the asm modules ported to C."""
import os, re, sys, shutil, subprocess
rc, resdir, out = sys.argv[1:4]
ver = sys.argv[4] if len(sys.argv) > 4 else None
res = os.path.join(out, 'res')
shutil.rmtree(res, ignore_errors=True); shutil.copytree(resdir, res)
fix = os.path.join(os.path.dirname(__file__), 'fix_ico.py')
for f in sorted(os.listdir(res)):
    if f.lower().endswith('.ico'):
        subprocess.check_call([sys.executable, fix, os.path.join(res, f), os.path.join(res, f)])
s = open(rc, 'rb').read().decode('latin1').replace('\r\n', '\n').replace('\r', '\n')
s = re.sub(r'\nIDD_CONTROLLER DLGINIT\nBEGIN\n.*?\nEND\n', '\n', s, flags=re.S)
s = s.replace('"resource\\\\', '"' + res + '/')
if ver:
    d = dict(re.findall(r'#define (GENS64_\w+) "([^"]*)"', open(ver).read()))
    # The About box: "Gens64 2.20 WIP", (revision, x86_64|x86), forum link, systems (whole text of the CTEXT)
    old = '"Gens Re-Recording v11c\\n\\nSega Genesis / Megadrive,\\nSega CD / Mega CD,\\nSega 32X emulator\\n\\nWin 9X / ME / 2000 / XP"'
    assert old in s, 'About box text not found'
    title = d.get('GENS64_TITLE', 'Gens64 (%s)' % d['GENS64_REV'])
    s = s.replace(old, '"%s\\nhttps://www.emu-land.net/forum/index.php?topic=93643\\n\\nSega Genesis / Mega Drive,\\nSega CD / Mega CD,\\nSega 32X emulator"' % title.replace(' (', '\\n('))
open(os.path.join(out, 'Gens_build.rc'), 'wb').write(s.encode('latin1'))
