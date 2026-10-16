"""Turn the translation dictionary into the C++ table that I18n.cpp looks up.

The table is data, not code: it is 397 pairs of two string literals, and hand
writing it would get one of the 237 index aligned entries wrong without saying
so. Generating it also lets the array be emitted in sort order, which is what
lets the lookup bisect instead of walking the whole list on every frame.

Only the pairs are emitted. The id a window keeps its place in the layout file
under is put on at run time by I18n::windowTitle, which appends the English
after a `###` marker; nothing about it belongs in the table, and a title stored
here with a marker in it would end up carrying two.

    python tools/i18n/i18n_gen.py     # writes src/I18n/I18nTable.inc

The dictionary it reads is written by `i18n_table.py`; run that one first when
the translations changed, and run this one last.
"""

import io
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DICTIONARY = os.path.join(HERE, "i18n_dict.json")
OUTPUT = os.path.join(ROOT, "src", "I18n", "I18nTable.inc")

# The docked windows, whose titles reach the layout file.
WINDOW_TITLES = [
    "Trace Viewer",
    "Trace Plots",
    "Task Viewer",
    "Var Viewer",
    "Plots",
    "Flash Output",
    "Recorder",
    "Statistics",
]

HEADER = """/* Generated from tools/i18n/i18n_dict.json by tools/i18n/i18n_gen.py.
 *
 * Do not edit: change the dictionary and run the generator, or the table and
 * the dictionary drift apart and the difference is invisible at run time,
 * because a missing entry leaves the English in place.
 *
 * The entries are sorted by the English text, which is the order the lookup
 * bisects.
 */
"""


def escape(text):
    """The text goes into a C++ literal as it stands.

    Escape sequences in it are already the ones the source spelling uses - a
    string that ends in a newline is stored as backslash n, the two characters,
    which is what C++ reads back as a newline. Escaping again would turn it
    into a literal backslash followed by an n.
    """
    if '"' in text or "\n" in text or "\\" in text.replace("\\n", "").replace("\\t", "").replace("\\\\", ""):
        raise ValueError("needs escaping by hand: %r" % text)
    return text


def docked_windows():
    """The titles the source hands to ImGui::Begin, which are the docked ones.

    Read from the source rather than kept as a list to compare against, because
    the list is the thing that goes stale: a window added later would silently
    keep an English id and lose its place in the saved layout.
    """
    # The title is read through the table, so the literal sits inside a call to
    # it. Every spelling the source may use is matched, because the generator
    # has to keep working after the wrapping pass has been run over the file.
    pattern = re.compile(r'ImGui::Begin\(\s*(?:I18n::(?:tr|windowTitle)\(\s*)?"((?:[^"\\]|\\.)*)"')
    found = set()

    for root in ("src/Gui", "src/ImguiPlugins"):
        for base, _dirs, names in os.walk(os.path.join(ROOT, root)):
            for name in sorted(names):
                if not name.endswith((".cpp", ".hpp")):
                    continue
                text = io.open(os.path.join(base, name), encoding="utf-8").read()
                found.update(pattern.findall(text))

    return found


def main():
    data = json.load(io.open(DICTIONARY, encoding="utf-8"))
    table = dict(data["translations"])

    in_source = docked_windows()

    for title in sorted(in_source):
        if title not in WINDOW_TITLES:
            print("a docked window is missing from WINDOW_TITLES: %s" % title)
            return 1

    for title in WINDOW_TITLES:
        if title not in in_source:
            print("WINDOW_TITLES names a window the source no longer opens: %s" % title)
            return 1
        if title not in table:
            print("window title not in the dictionary: %s" % title)
            return 1

        # The title is translated like any other text here, and nothing else is
        # added to it. What the window is filed under in the layout file is put
        # on at run time, by I18n::windowTitle: it appends the English after a
        # `###` marker, which ImGui cuts the name at for display and restarts
        # its hash at for the identifier. One layout therefore serves every
        # language, and a pane cannot end up where the other language left it.
        #
        # A title stored here with the marker already in it would come out
        # carrying two of them, and the second would be the one that counted.

    lines = [HEADER, "static const Entry table[] = {\n"]
    for english in sorted(table):
        lines.append('\t{"%s", "%s"},\n' % (escape(english), escape(table[english])))
    lines.append("};\n")

    directory = os.path.dirname(OUTPUT)
    if not os.path.isdir(directory):
        os.makedirs(directory)
    io.open(OUTPUT, "w", encoding="utf-8", newline="\n").write("".join(lines))

    print("entries: %d" % len(table))
    print("docked windows translated: %d" % len(WINDOW_TITLES))
    print("written: %s" % os.path.relpath(OUTPUT, ROOT).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
