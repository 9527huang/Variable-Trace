"""Report prose in the interface files that the table does not carry.

The wrapping pass only knows the shapes it was taught, and the extraction pass
only reports what it looked at. Neither can tell "this literal was never
wrapped" from "this literal is a format string", so both stay quiet about a
label that was written in a way they do not recognise, and the English simply
stays on screen.

This asks the question the other way round: which literals read like something
a person is meant to see, and are neither in the table nor handed to it. What
comes out is a list to look at, not a list of faults - an id, an OpenGL shader,
a file filter and a printf format all read like prose by this measure and none
of them is text.

    python tools/i18n/i18n_review.py tools/i18n/i18n_dict.json

Run it last, after the wrapping pass and the extraction pass have both gone
quiet: neither of them can see a label written in a shape they do not know.
"""

import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ROOTS = [os.path.join(ROOT, "src", "Gui"), os.path.join(ROOT, "src", "ImguiPlugins")]

LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')

# At least two words, which is what most prose has and most identifiers do not.
PROSE = re.compile(r"^[^ ]*[A-Za-z][^ ]* [^ ]*[A-Za-z]")

# A literal right after one of these is already going through the table.
THROUGH_TABLE = re.compile(r"(?:I18n::tr|padToColumn|drawTextAlignedToSize|drawCenteredText)\s*\(\s*$")


def literals(text):
    """Yields (value, goes_through_the_table) for every literal outside a comment."""
    for line in text.splitlines():
        stripped = line.lstrip()

        if stripped.startswith(("//", "*", "/*", "#")):
            continue

        # Drop a trailing comment, so that prose in it is not read as a string.
        body = line.split("//", 1)[0]

        previous_end = -1

        for match in LITERAL.finditer(body):
            value = match.group(0)[1:-1]
            before = body[: match.start()]

            # An adjacent literal is the rest of the one before it - C++ joins
            # them before anything else sees them - so only the first piece is
            # a thing to look for.
            adjacent = (previous_end != -1 and before.strip() == "")
            previous_end = match.end()

            if adjacent or not PROSE.match(value):
                continue

            # A literal handed straight to the table is already covered, and
            # the table's answer is what the reader sees.
            yield value, bool(THROUGH_TABLE.search(before))


def main():
    dictionary = json.load(io.open(sys.argv[1], encoding="utf-8"))
    table = dictionary["translations"] if "translations" in dictionary else dictionary

    unwrapped = []

    for root in ROOTS:
        for base, _dirs, names in os.walk(root):
            for name in sorted(names):
                if not name.endswith((".cpp", ".hpp")):
                    continue

                full = os.path.join(base, name)
                path = os.path.relpath(full, ROOT).replace("\\", "/")

                for value, through_table in literals(io.open(full, encoding="utf-8").read()):
                    if value in table or through_table:
                        continue

                    unwrapped.append((path, value))

    print("prose the table does not carry: %d" % len(unwrapped))
    for path, value in unwrapped:
        print("  %-30s %s" % (path, value[:120]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
