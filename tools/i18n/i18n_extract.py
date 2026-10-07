"""Collect the interface text that has to be translated.

The application is written in English and every screen string is meant to run
through `tr`, so the list produced here is the authority for two things at
once: the translation table, and the check that no call site was left without
one. Only literals that can reach the screen are collected - an id or a path
is not text anybody reads.

    python tools/i18n/i18n_extract.py [out.json]

The paths below are resolved from this file, so the pass gives the same answer
whatever directory it is run from.
"""

import io
import os
import re
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ROOTS = [os.path.join(ROOT, "src", "Gui"), os.path.join(ROOT, "src", "ImguiPlugins")]

# Literal as the first argument.
FIRST = re.compile(
    r'\b(?:ImGui::|ImGui::)(Text|TextUnformatted|TextDisabled|TextWrapped|LabelText|'
    r'Button|SmallButton|InvisibleButton|Checkbox|RadioButton|Selectable|MenuItem|'
    r'Begin|BeginChild|BeginMenu|BeginCombo|BeginTabItem|Combo|InputText|InputTextWithHint|'
    r'InputInt|InputFloat|InputDouble|DragInt|DragFloat|BeginTooltip|BulletText|'
    r'CollapsingHeader|TreeNodeEx|TreeNode|MenuItem|SetNextItemWidth)\s*\(\s*"((?:[^"\\]|\\.)*)"'
)

# Literals in a later argument of a call whose first argument is a literal id.
LATER = re.compile(
    r'\bImGui::(?:TextColored|TextDisabled|Text|SetTooltip|HelpMarker|LabelText)\s*\(\s*'
    r'(?:[^,()]+,\s*)?"((?:[^"\\]|\\.)*)"'
)

HELPER = re.compile(r'\b(?:HelpMarker|showDeletePopup|drawTextAlignedToSize)\s*\(\s*"((?:[^"\\]|\\.)*)"')

QUESTION = re.compile(r'\bshowQuestionBox\s*\(\s*"[^"]*"\s*,\s*"((?:[^"\\]|\\.)*)"')

COLORED = re.compile(r'\bImGui::TextColored\s*\([^,]+,\s*"((?:[^"\\]|\\.)*)"')

# Anything that is plainly an identifier rather than text.
def is_id(text):
    stripped = text.lstrip("#")
    return stripped == "" or text.startswith("##") or text.startswith("###")


def collect():
    found = {}
    for root in ROOTS:
        for base, _dirs, names in os.walk(root):
            for name in names:
                if not name.endswith((".cpp", ".hpp")):
                    continue
                full = os.path.join(base, name)
                path = os.path.relpath(full, ROOT).replace("\\", "/")
                source = io.open(full, encoding="utf-8").read()
                for pattern in (FIRST, HELPER, QUESTION, COLORED, LATER):
                    for match in pattern.finditer(source):
                        text = match.group(match.lastindex)
                        if is_id(text):
                            continue
                        found.setdefault(text, []).append(path)
    return found


found = collect()
print("candidates: %d" % len(found))
for text in sorted(found):
    print("  %-95s %s" % (text[:95], sorted(set(found[text]))[0]))

if len(sys.argv) > 1:
    with io.open(sys.argv[1], "w", encoding="utf-8") as handle:
        json.dump(sorted(found), handle, ensure_ascii=False, indent=1)
    print("\nwritten to %s" % sys.argv[1])
