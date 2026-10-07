"""Every string literal in the interface files, so the ones the first pass
missed can be looked at rather than guessed at.

C++ joins adjacent literals before the compiler sees them, so a message split
over several lines arrives here as several pieces. They are printed with their
line numbers and left joined so that they read as the source does.

    python tools/i18n/i18n_leftovers.py [i18n_sites.json]

Given the list the first pass captured, it prints the literals that list does
not hold, which is the set to look through after a pass reports nothing left to
translate but the interface still shows English.
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ROOTS = [os.path.join(ROOT, "src", "Gui"), os.path.join(ROOT, "src", "ImguiPlugins")]

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')

captured = set()
if len(sys.argv) > 1:
    import json
    captured = set(json.load(io.open(sys.argv[1], encoding="utf-8")))

# Things that are plainly not shown to a reader.
SKIP = re.compile(
    r"^(#|%[a-zA-Z]|\\n|$)"
)


def join(path, line):
    return "%s:%d" % (path, line)


rows = []
for root in ROOTS:
    for base, _dirs, names in os.walk(root):
        for name in sorted(names):
            if not name.endswith((".cpp", ".hpp")):
                continue
            full = os.path.join(base, name)
            path = os.path.relpath(full, ROOT).replace("\\", "/")
            text = io.open(full, encoding="utf-8").read()
            for index, line in enumerate(text.splitlines(), start=1):
                for match in LITERAL.finditer(line):
                    value = match.group(1)
                    if '"' in line[: match.start()] and line.count("/") > 0:
                        # a comment line
                        if line.lstrip().startswith(("//", "*", "/*")):
                            continue
                    rows.append((path, index, value))

print("total literals: %d" % len(rows))
left = [r for r in rows if r[2] not in captured]
print("not captured by the first pass: %d" % len(left))
for path, index, value in left:
    print("  %-30s %5d  %s" % (path, index, value[:110]))
