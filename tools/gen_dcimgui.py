#!/usr/bin/env python3
# /// script
# requires-python = ">=3.8"
# dependencies = ["ply"]
# ///
"""Regenerates lib/dcimgui/* from lib/imgui via lib/dear_bindings.

Run this after updating the lib/imgui submodule, then review the diff and commit.
`uv run tools/gen_dcimgui.py`
"""
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEAR_BINDINGS = REPO_ROOT / "lib" / "dear_bindings"
IMGUI = REPO_ROOT / "lib" / "imgui"
OUT = REPO_ROOT / "lib" / "dcimgui"

FORWARD_DECL_MARKER = "// Auto-generated forward declarations for C header"
TYPEDEF_RE = re.compile(r"typedef\s+(?:struct|union|enum)\s+\w+\s+(\w+);")
COND_OPEN_RE = re.compile(r"#\s*(?:if|ifdef|ifndef)\b")
COND_BRANCH_RE = re.compile(r"#\s*(?:else|elif)\b")
COND_CLOSE_RE = re.compile(r"#\s*endif\b")


def run(*args):
    subprocess.run(
        [sys.executable, "dear_bindings.py", *args],
        cwd=DEAR_BINDINGS,
        check=True,
    )


def strip_comments(lines):
    """Each line with its comments blanked out, so names mentioned in prose don't
    count as uses."""
    stripped = []
    in_block = False
    for line in lines:
        out = []
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    break
                in_block = False
                i = end + 2
            elif line.startswith("//", i):
                break
            elif line.startswith("/*", i):
                in_block = True
                i += 2
            else:
                out.append(line[i])
                i += 1
        stripped.append("".join(out))
    return stripped


def hoist_late_typedefs(path):
    """Move typedefs that the generator left behind their first use up into the
    forward-declaration block.

    dear_bindings only hoists forward declarations it generates itself: where
    Dear ImGui already forward-declares a type (e.g. `struct ImGuiTextFilterItem;`
    just above the struct holding an ImVector of it), the typedef stays put, which
    can leave it below the hoisted ImVector<> instantiation that needs it. See
    lib/dear_bindings/src/modifiers/mod_forward_declare_structs.py.
    """
    with open(path, "r", encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")
    code = strip_comments(lines)

    marker = next(i for i, l in enumerate(lines) if l.strip() == FORWARD_DECL_MARKER)

    # Extent of the block: the last typedef in the run of typedefs and comments
    # following the marker.
    block_end = marker
    declared = set()
    for i in range(marker + 1, len(lines)):
        text = lines[i].strip()
        match = TYPEDEF_RE.match(text)
        if match:
            block_end = i
            declared.add(match.group(1))
        elif text and not text.startswith("//"):
            break

    # Candidates: typedefs below the block sitting in the same conditional branch
    # as the block itself. Anything nested deeper is left alone, since the branch
    # it sits in may not be the one that compiles.
    branch = []
    marker_branch = None
    hoisted = []
    for i, line in enumerate(lines):
        text = line.strip()
        if COND_OPEN_RE.match(text):
            branch.append([i, 0])
        elif COND_BRANCH_RE.match(text):
            branch[-1][1] += 1
        elif COND_CLOSE_RE.match(text):
            branch.pop()
        elif i == marker:
            marker_branch = [tuple(b) for b in branch]
        elif i > block_end and [tuple(b) for b in branch] == marker_branch:
            match = TYPEDEF_RE.match(text)
            if match and match.group(1) not in declared:
                name = re.compile(r"\b" + match.group(1) + r"\b")
                # Preprocessor directives are skipped: a macro body naming the
                # type is only substituted where the macro is used.
                if any(
                    name.search(code[j])
                    for j in range(i)
                    if not lines[j].lstrip().startswith("#")
                ):
                    hoisted.append(i)

    if not hoisted:
        return

    moved = [lines[i] for i in hoisted]
    for i in reversed(hoisted):
        del lines[i]
    lines[marker + 1 : marker + 1] = moved

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(lines))

    print("{}: hoisted {}".format(path.name, ", ".join(l.strip() for l in moved)))


def main():
    run("-o", str(OUT / "dcimgui"), str(IMGUI / "imgui.h"))
    run("-o", str(OUT / "dcimgui_internal"), "--include", str(IMGUI / "imgui.h"),
        str(IMGUI / "imgui_internal.h"))

    for header in ("dcimgui.h", "dcimgui_internal.h"):
        hoist_late_typedefs(OUT / header)


if __name__ == "__main__":
    main()
