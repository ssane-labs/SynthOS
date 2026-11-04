#!/usr/bin/env python3
"""Every built-in function must have one call gate signature: a function declared in one source and defined in
another gets an entry from both, and a 32-bit program's stack breaks if they disagree on the bytes popped.

usage: w32sigcheck.py SIG.h..."""
import re
import sys

seen, bad = {}, []
for path in sys.argv[1:]:
    for m in re.finditer(r'\{\(const void \*\)(\w+), (\d+), (\d+), (\d+), "([^"]*)"\}', open(path).read()):
        name, sig = m.group(1), m.group(2, 3, 4, 5)
        if name in seen and seen[name][0] != sig:
            bad.append('%s: %s pops %s here, %s in %s' % (path, name, sig[0], seen[name][0][0], seen[name][1]))
        seen.setdefault(name, (sig, path))
if bad:
    sys.stderr.write('\n'.join('w32sigcheck: ' + b for b in bad) + '\n')
    sys.exit(1)
