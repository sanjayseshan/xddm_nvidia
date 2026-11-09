#!/usr/bin/env python3
"""Reject DLL imports which prevent XP x86 from loading a GDI driver."""
import re
import subprocess
import sys

output = subprocess.check_output(['i686-w64-mingw32-objdump', '-p', sys.argv[1]], text=True)
imports = {name.lower() for name in re.findall(r'DLL Name:\s+(\S+)', output)}
if imports != {'win32k.sys'}:
    sys.exit(f'XP x86 display DLL must import only WIN32K.SYS; got {sorted(imports)}')
print('XP x86 display import check passed (WIN32K.SYS only).')
