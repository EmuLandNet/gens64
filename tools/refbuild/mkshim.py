#!/usr/bin/env python3
"""Generate the include shim for building the Windows-era sources on Linux:
case-insensitive aliases for quoted #includes, svnrev.h, StdAfx.h and MyAES.h for 7z, BaseTsd.h.
Usage: mkshim.py <Gens-rr dir> <out dir> <svn rev> <mingw include dir>"""
import os, re, glob, sys, shutil
root, ci, rev, mingw_inc = sys.argv[1:5]
os.makedirs(ci, exist_ok=True)
def write(name, text):
    p = os.path.join(ci, name)
    if not os.path.exists(p) or open(p).read() != text:
        open(p, 'w').write(text)
write('svnrev.h', '#define SVN_REV "%s"\n' % rev)
write('StdAfx.h', '#ifndef __STDAFX_H\n#define __STDAFX_H\n#include "%s/7z/CPP/Common/MyWindows.h"\n#include "%s/7z/CPP/Common/NewHandler.h"\n#endif\n' % (root, root))
write('MyAES.h', '#include "%s/7z/CPP/7zip/Crypto/MyAes.h"\n' % root)
links = {'BaseTsd.h': os.path.join(mingw_inc, 'basetsd.h')}
dirs = [root+'/src', root+'/dependencies', root+'/7z/CPP', root+'/7z', root]
index = {}
for d in [root+'/src', root+'/dependencies', root+'/dependencies/DirectX', root+'/dependencies/lua/src', root+'/dependencies/mp3_dec']:
    for f in sorted(os.listdir(d)): index.setdefault(f.lower(), os.path.join(d, f))
for s in sorted(glob.glob(root+'/src/*.[ch]') + glob.glob(root+'/src/*.cpp') + glob.glob(root+'/dependencies/mp3_dec/*')):
    for inc in re.findall(r'#\s*include\s*"([^"]+)"', open(s, errors='replace').read()):
        if os.path.exists(os.path.join(os.path.dirname(s), inc)) or any(os.path.exists(os.path.join(d, inc)) for d in dirs): continue
        key = inc.replace('\\', '/').split('/')[-1].lower()
        if key in index: links[inc] = index[key]
for name, target in links.items():
    p = os.path.join(ci, name)
    if os.path.lexists(p):
        if os.path.islink(p) and os.readlink(p) == target: continue
        os.remove(p)
    os.symlink(target, p)
shutil.copyfile(os.path.join(os.path.dirname(__file__), 'msvc_compat.h'), os.path.join(ci, 'msvc_compat.h'))
