"""Find every interface string and, on request, wrap it in I18n::tr.

One pass serves three purposes: it lists what has to be translated, it rewrites
the call sites so that they ask for a translation, and it reports any string it
saw that has no translation yet - which is what keeps the table honest, because
a missing entry is invisible at run time: the English simply stays.

Only the shapes that can put text on the screen are touched. An id that starts
with `##` is not text, and a format string is translated as it stands so that
its placeholders keep their order.

    python tools/i18n/i18n_apply.py list                    # what is there
    python tools/i18n/i18n_apply.py apply tools/i18n/i18n_dict.json
    python tools/i18n/i18n_apply.py check tools/i18n/i18n_dict.json

`apply` is the first step when a new string has to be translated: it wraps the
call sites, and whatever it reports as having no translation is the work left
for `i18n_table.py`.
"""

import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ROOTS = [os.path.join(ROOT, "src", "Gui"), os.path.join(ROOT, "src", "ImguiPlugins")]

# Calls whose first argument is the text a reader sees. `Combo` and the others
# take a label there even though they also take an id, in the form
# `Text##id`, which is why the whole literal is wrapped rather than half of it.
VISIBLE_FIRST = (
    "Text|TextUnformatted|TextDisabled|TextWrapped|LabelText|Button|SmallButton|"
    "Checkbox|RadioButton|Selectable|MenuItem|Begin|BeginChild|BeginMenu|BeginCombo|"
    "BeginTabItem|Combo|InputText|InputTextWithHint|InputInt|InputFloat|DragInt|"
    "DragFloat|CollapsingHeader|TreeNode|TreeNodeEx|BulletText|SetTooltip|OpenPopup|"
    "BeginPopupModal|BeginPopup"
)

FIRST = re.compile(
    r"(?P<call>\bImGui::(?P<name>" + VISIBLE_FIRST + r"))\s*\(\s*(?P<lit>\"(?:[^\"\\]|\\.)*\")"
)

HELPERS = re.compile(
    r"(?P<call>\b(?:HelpMarker|showDeletePopup|drawTextAlignedToSize|drawCenteredText))\s*\(\s*"
    r"(?P<lit>\"(?:[^\"\\]|\\.)*\")"
)

# The colour is an expression rather than a name - `GuiHelper::orangeLight()`,
# `ImVec4(1, 0, 0, 1)` - so the first argument is matched as a run of
# parenthesised groups and plain characters, and stops at the comma that
# separates it from the text.
COLORED = re.compile(r"(?P<call>\bImGui::TextColored)\s*\(\s*(?:(?:\([^()]*\)|[^,()])+)\s*,\s*(?P<lit>\"(?:[^\"\\]|\\.)*\")")

# A tooltip whose text is the second argument, because the first is the `%s`
# it is printed through.
TOOLTIP_TEXT = re.compile(r"(?P<call>\bImGui::SetTooltip\s*\(\s*\"%s\"\s*,\s*)(?P<lit>\"(?:[^\"\\]|\\.)*\")")

QUESTION = re.compile(
    r"(?P<call>\bshowQuestionBox)\s*\(\s*\"(?:[^\"\\]|\\.)*\"\s*,\s*(?P<lit>\"(?:[^\"\\]|\\.)*\")"
)

TITLE = re.compile(r"(?P<call>glfwCreateWindow\s*\([^,]+,\s*[^,]+,\s*)(?P<lit>\"(?:[^\"\\]|\\.)*\")")

# A popup carries a title of its own, and both halves of the call are read by
# the user. The message comes second, which is why it needs its own pattern.
POPUP_TITLE = re.compile(r"(?P<call>\b\w*[Pp]opup\.show\s*\(\s*)(?P<lit>\"(?:[^\"\\]|\\.)*\")")
POPUP_MESSAGE = re.compile(r"(?P<call>\b\w*[Pp]opup\.show\s*\(\s*\"(?:[^\"\\]|\\.)*\"\s*,\s*)(?P<lit>\"(?:[^\"\\]|\\.)*\")")

# A message built into a variable first and drawn later. Only the ones that
# read as a sentence are taken: the same shape initialises a path or a shader
# source, and neither is text a reader sees.
DECLARED = re.compile(
    r"(?P<call>\b(?:const\s+)?(?:char\s*\*\s*const|const\s+char\s*\*|std::string)\s+\w+\s*=\s*)"
    r"(?P<lit>\"(?:[^\"\\]|\\.)*\")"
)

PATTERNS = [FIRST, HELPERS, COLORED, QUESTION, TITLE, POPUP_TITLE, POPUP_MESSAGE, DECLARED, TOOLTIP_TEXT]


def is_id(text):
    return text.startswith("##") or text.startswith("###") or text == ""


# At least two words, which is what a sentence has and what a path, a shader
# and a format string generally do not.
PROSE = re.compile(r"[A-Za-z][^ ]* [^ ]*[A-Za-z]")

# Long messages split over two literals because the source line would not hold
# them. C++ joins the pieces into one string, so wrapping the first piece alone
# leaves the call broken. They are collected rather than rewritten.
ADJACENT = []


def shown(path):
    """A path as the reports print it: from the repository root, with slashes.

    The passes walk absolute paths now that they are run from wherever they are
    kept, and a report nobody can read is worse than no report.
    """
    return os.path.relpath(path, ROOT).replace("\\", "/")


def files():
    for root in ROOTS:
        for base, _dirs, names in os.walk(root):
            for name in sorted(names):
                if name.endswith((".cpp", ".hpp")):
                    yield os.path.join(base, name)


def files_holding_wording():
    """Every source file that may quote a piece of interface text.

    Wider than the files that are rewritten, because a phrase can be written
    where it belongs to the data and only reach the screen later - the
    persistence layer describes why a project would not open, and the message
    is put through the table at the call that shows it. The generated table
    itself is left out, or every entry would look used.
    """
    for base, _dirs, names in os.walk(os.path.join(ROOT, "src")):
        for name in sorted(names):
            if not name.endswith((".cpp", ".hpp")):
                continue

            path = os.path.join(base, name)

            if shown(path).startswith("src/I18n/"):
                continue

            yield path


def scan(text):
    """Returns [(start, end, literal, call)] for every site, in order."""
    sites = []
    for pattern in PATTERNS:
        for match in pattern.finditer(text):
            literal = match.group("lit")
            body = literal[1:-1]

            if is_id(body):
                continue

            # The declared shape is used for paths and shader sources as well,
            # so only the ones that read as a sentence are taken from it.
            if pattern is DECLARED and not PROSE.search(body):
                continue

            # An adjacent literal is the rest of the one before it, so the
            # whole message is not where this pattern found it. C++ joins the
            # pieces before any call sees them, so wrapping the first piece
            # alone does not compile and the call is left for a hand to join.
            # The second piece almost always sits on the next line, which is
            # why the whitespace is skipped rather than only a quote looked for.
            if re.match(r"\s*\"", text[match.end("lit") :]):
                ADJACENT.append(body)
                continue

            sites.append((match.start("lit"), match.end("lit"), literal, match.group("call")))
    sites.sort()
    return sites


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "list"
    dictionary = {}
    if command in ("apply", "check"):
        dictionary = json.load(io.open(sys.argv[2], encoding="utf-8"))

        # The dictionary keeps the translations beside the list of strings that
        # were deliberately left alone, so that the two can be read together
        # when something needs checking.
        if "translations" in dictionary:
            dictionary = dictionary["translations"]

    if command == "check":
        # A translation nothing asks for is either a string that was removed
        # from the interface and left behind here, or one whose call site was
        # never wrapped - and the second is invisible at run time, because the
        # English simply stays on screen.
        #
        # A message the source splits over two literals is not found by reading
        # the file as it stands, so the two pieces are joined first, the way the
        # compiler joins them.
        wanted = {}
        for path in files_holding_wording():
            text = io.open(path, encoding="utf-8").read()
            joined = re.sub(r'"\s*"', "", text)
            for english in dictionary:
                if ('"' + english + '"') in text or ('"' + english + '"') in joined:
                    wanted[english] = shown(path)

        unused = [english for english in dictionary if english not in wanted]

        print("translations: %d" % len(dictionary))
        print("never asked for: %d" % len(unused))
        for english in sorted(unused):
            print("  %r" % english)
        return

    seen = {}
    missing = {}
    changed_files = []

    for path in files():
        text = io.open(path, encoding="utf-8").read()
        sites = scan(text)
        for _start, _end, literal, _call in sites:
            seen.setdefault(literal[1:-1], []).append(shown(path))

        if command != "apply":
            continue

        # Rewrite back to front so the earlier offsets stay valid.
        rebuilt = text
        for start, end, literal, _call in reversed(sites):
            key = literal[1:-1]
            if key not in dictionary:
                missing.setdefault(key, []).append(shown(path))
                continue
            rebuilt = rebuilt[:start] + "I18n::tr(" + literal + ")" + rebuilt[end:]

        if rebuilt != text:
            io.open(path, "w", encoding="utf-8", newline="").write(rebuilt)
            changed_files.append(shown(path))

    if command == "apply":
        print("files rewritten: %d" % len(changed_files))
        for path in changed_files:
            print("  " + path)
        print("\nstrings with no translation: %d" % len(missing))
        for key in sorted(missing):
            print("  %s   <- %s" % (key[:100], sorted(set(missing[key]))[0]))
        if ADJACENT:
            print("\nmessages split over two literals, to be joined by hand: %d" % len(ADJACENT))
            for key in sorted(set(ADJACENT)):
                print("  %s" % key[:100])
        return

    print("interface strings: %d" % len(seen))
    for key in sorted(seen):
        print("  %-100s %s" % (key[:100], sorted(set(seen[key]))[0]))
    if len(sys.argv) > 2:
        json.dump(sorted(seen), io.open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
