"""Give every file that asks the table for text the header that declares it.

A call site wrapped in `I18n::tr` does not compile without `#include
"I18n.hpp"`, and there are more call sites than there are files, spread over
the whole of src/Gui. Adding the include by hand means reading sixteen include
blocks and getting the sort order right in each; doing it here means the rule
is written down once.

The include goes into the block of quoted includes the file already has, in
sorted position, so that the blocks keep the order they were written in. A file
that has no such block gets it after its last include.

    python tools/i18n/i18n_includes.py            # report
    python tools/i18n/i18n_includes.py apply      # write
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INCLUDE = '#include "I18n.hpp"'

# Trailing whitespace is matched as spaces and tabs rather than as `\s`, because
# `\s` reaches across the newline and would put the end of the match at the
# start of the next directive.
QUOTED = re.compile(r'^#include[ \t]+"[^"]+"[ \t]*$', re.MULTILINE)


def files():
    for base, _dirs, names in os.walk(os.path.join(ROOT, "src")):
        for name in sorted(names):
            if name.endswith((".cpp", ".hpp")):
                yield os.path.join(base, name)


def insertion_point(text):
    """The offset the include belongs at, or None when it is already there."""
    if INCLUDE in text:
        return None

    matches = list(QUOTED.finditer(text))
    if not matches:
        last = text.rfind("#include")
        if last == -1:
            return 0
        return text.find("\n", last) + 1

    # The block of quoted includes: every match that follows the previous one
    # without anything in between.
    block = [matches[0]]
    for match in matches[1:]:
        if text[block[-1].end() : match.start()].strip() == "":
            block.append(match)
        else:
            break

    for match in block:
        if match.group(0) > INCLUDE:
            return match.start()

    return block[-1].end() + 1


def main():
    write = len(sys.argv) > 1 and sys.argv[1] == "apply"
    touched = []

    for path in files():
        text = io.open(path, encoding="utf-8").read()
        if "I18n::tr(" not in text:
            continue

        offset = insertion_point(text)
        if offset is None:
            continue

        touched.append(os.path.relpath(path, ROOT).replace("\\", "/"))
        if not write:
            continue

        io.open(path, "w", encoding="utf-8", newline="").write(text[:offset] + INCLUDE + "\n" + text[offset:])

    print("files needing the include: %d" % len(touched))
    for name in touched:
        print("  " + name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
