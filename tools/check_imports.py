#!/usr/bin/env python3
"""Checks that a 32-bit Windows binary imports only functions that Windows XP SP3 has.

Usage: check_imports.py <objdump> <exe or dll> <list>

The list (tools/xp_imports.txt) has one "DLL!Function" per line; it holds the imports of the builds
that were run on Windows XP. A new import that is not in it fails the build: look up its minimum
supported Windows (Microsoft's documentation of the function, or the DLL's exports on XP) and add it
to the list only if XP has it. Exits 1 and prints the imports that are not in the list.
"""
import re
import subprocess
import sys


def imports(objdump, path):
    out = subprocess.run([objdump, '-p', path], capture_output=True, text=True, check=True).stdout
    dll = None
    found = set()
    for line in out.splitlines():
        m = re.match(r'\s*DLL Name:\s*(\S+)', line)
        if m:
            dll = m.group(1).lower()
            continue
        m = re.match(r'\s+(\d+)\s+(\S+)\s*$', line)
        if dll and m:
            found.add(dll + '!' + m.group(2))
    return found


def main():
    objdump, path, listfile = sys.argv[1:4]
    known = set()
    with open(listfile) as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                dll, _, name = line.partition('!')
                known.add(dll.lower() + '!' + name)
    new = sorted(imports(objdump, path) - known)
    if new:
        print('%s imports functions that are not in %s (Windows XP):' % (path, listfile))
        for name in new:
            print('  ' + name)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
