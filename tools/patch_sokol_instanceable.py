#!/usr/bin/env python3
"""Make sokol_gfx.h / sokol_imgui.h instanceable via an ambient "current context".

Rather than rewriting the ~thousands of `_sg.` / `_simgui.` member accesses (risky,
and a regex would mangle `&_sg`, `sizeof(_simgui)`, strings & comments), this turns
the single global STATE VALUE into a thread-local STATE POINTER and defines the old
name as a macro that dereferences it:

    static _sg_state_t  _sg;            ->   static THREADLOCAL _sg_state_t* _sg_current;
                                             #define _sg (*_sg_current)

Every existing use is then rewritten *by the C preprocessor*, correctly and by
construction:
    _sg.foo        -> (*_sg_current).foo
    &_sg.foo       -> &(*_sg_current).foo          (address of a member: correct)
    &_sg           -> &(*_sg_current) == _sg_current (whole-struct pointer: correct)
    sizeof(_sg)    -> sizeof(*_sg_current)          (struct size, NOT pointer: correct)

This mirrors Dear ImGui's `GImGui` + SetCurrentContext() pattern. Each plugin
instance does:  void* c = sg_make_context(); sg_set_current_context(c); sg_setup(...)
and sets its context current at the top of each frame before touching sokol.

Each edit is an anchored find->replace keyed on a stable source string (not a line
number). `apply` skips any edit whose result is already present, so it is idempotent
and re-runnable after a sokol update -- and will add only newly-introduced edits (e.g.
the Metal completion-handler fix) to an already-patched tree. The output is byte-for-
byte the `multi-instance` fork diff against upstream; there are no marker comments. The
C compiler is the acceptance test: build each backend (-DSOKOL_METAL, -DSOKOL_D3D11,
...) and a green build means the transform landed.

CAVEATS (see the chat discussion):
  * `_sg_current` is thread-local (SOKOL_INSTANCE_THREADLOCAL): every thread that calls
    into sokol must set its current context before its first call. A GPU callback that
    runs on a foreign thread must NOT read `_sg` (it would be null there) -- the Metal
    completion handler is patched to capture its semaphore in a local for exactly this
    reason.
  * The per-context struct is allocated with calloc/free, NOT sokol's per-context
    sg_allocator (which lives *inside* the struct -- chicken/egg). Swap in your own
    allocator in sg_make_context/sg_destroy_context if you need to.
  * sokol_imgui.h's `_simgui` is a separate global and gets the same treatment here.
    Any other util header you adopt (sokol_gl's `_sgl`, etc.) needs its own pass.

Usage:
    python tools/patch_sokol_instanceable.py               # apply + print diff
    python tools/patch_sokol_instanceable.py --dry-run     # print diff only, no write
    python tools/patch_sokol_instanceable.py --verify      # apply, then compile-check
"""

import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile

# Resolve sokol path relative to this script (tools/ -> ../lib/sokol).
_HERE = os.path.dirname(os.path.abspath(__file__))
_SOKOL = os.path.normpath(os.path.join(_HERE, "..", "lib", "sokol"))


# Structural fix for the Metal backend's frame-completion handler. The `find` side is
# the verbatim upstream block (so the anchor matches a pristine checkout); the `replace`
# side hoists the semaphore into a local (the block runs on a foreign thread where the
# thread-local `_sg` is null, so it must not deref `_sg`).
_MTL_SEM_FIX = (
    "        [_sg.mtl.cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> cmd_buf) {\n"
    "            // NOTE: this code is called on a different thread!\n"
    "            _SOKOL_UNUSED(cmd_buf);\n"
    "            dispatch_semaphore_signal(_sg.mtl.sem);\n"
    "        }];\n",
    "        dispatch_semaphore_t completion_sem = _sg.mtl.sem;\n"
    "        [_sg.mtl.cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> cmd_buf) {\n"
    "            // NOTE: this code is called on a different thread!\n"
    "            _SOKOL_UNUSED(cmd_buf);\n"
    "            dispatch_semaphore_signal(completion_sem);\n"
    "        }];\n",
)


# Per-file recipe. `proto_after` and `state_decl` are exact source strings we anchor on;
# the prototype + impl blocks are generated from the remaining fields. `extra_edits` are
# already-formed (find, replace) pairs for anything beyond the global->pointer swap.
TARGETS = [
    {
        "path": os.path.join(_SOKOL, "sokol_gfx.h"),
        "proto_after": "SOKOL_GFX_API_DECL void sg_shutdown(void);",
        "state_decl": "static _sg_state_t _sg;",
        "api_decl": "SOKOL_GFX_API_DECL",
        "prefix": "sg",
        "state_type": "_sg_state_t",
        "macro_name": "_sg",
        "ptr_name": "_sg_current",
        "extra_edits": [_MTL_SEM_FIX],
    },
    {
        "path": os.path.join(_SOKOL, "util", "sokol_imgui.h"),
        "proto_after": "SOKOL_IMGUI_API_DECL void simgui_shutdown(void);",
        "state_decl": "static _simgui_state_t _simgui;",
        "api_decl": "SOKOL_IMGUI_API_DECL",
        "prefix": "simgui",
        "state_type": "_simgui_state_t",
        "macro_name": "_simgui",
        "ptr_name": "_simgui_current",
        "extra_edits": [],
    },
]


def _protos(t):
    a, p = t["api_decl"], t["prefix"]
    return (
        f"{a} void* {p}_make_context(void);\n"
        f"{a} void  {p}_set_current_context(void* ctx);\n"
        f"{a} void* {p}_get_current_context(void);\n"
        f"{a} void  {p}_destroy_context(void* ctx);"
    )


def _impl(t):
    p, st = t["prefix"], t["state_type"]
    mac, ptr = t["macro_name"], t["ptr_name"]
    return (
        "#include <stdlib.h>\n"
        "#ifndef SOKOL_INSTANCE_THREADLOCAL\n"
        "  #if defined(_MSC_VER)\n"
        "    #define SOKOL_INSTANCE_THREADLOCAL __declspec(thread)\n"
        "  #else\n"
        "    #define SOKOL_INSTANCE_THREADLOCAL _Thread_local\n"
        "  #endif\n"
        "#endif\n"
        f"static SOKOL_INSTANCE_THREADLOCAL {st}* {ptr};\n"
        f"#define {mac} (*{ptr})\n"
        f"SOKOL_API_IMPL void* {p}_make_context(void) {{ return calloc(1, sizeof({st})); }}\n"
        f"SOKOL_API_IMPL void  {p}_set_current_context(void* ctx) {{ {ptr} = ({st}*)ctx; }}\n"
        f"SOKOL_API_IMPL void* {p}_get_current_context(void) {{ return {ptr}; }}\n"
        f"SOKOL_API_IMPL void  {p}_destroy_context(void* ctx) {{ free(ctx); }}"
    )


def _edits(t):
    """Ordered (find, replace) structural edits for a target. apply does find->replace,
    skipping any whose replace is already present (idempotent)."""
    return [
        # Add the context-API prototypes right after the shutdown declaration.
        (t["proto_after"], t["proto_after"] + "\n" + _protos(t)),
        # Swap the global state VALUE for a thread-local pointer + macro + context API.
        (t["state_decl"], _impl(t)),
        *t["extra_edits"],
    ]


def _detect_nl(text):
    """The file's dominant line ending. sokol checked out on Windows is CRLF; our edit
    templates are authored with '\\n', so we translate them to match and keep the
    round-trip byte-exact."""
    return "\r\n" if "\r\n" in text else "\n"


def apply_patch(text, t):
    nl = _detect_nl(text)
    for find, repl in _edits(t):
        find = find.replace("\n", nl)
        repl = repl.replace("\n", nl)
        if repl in text:
            continue  # edit already present -> idempotent
        if find not in text:
            raise SystemExit(
                f"  anchor not found in {os.path.basename(t['path'])}:\n"
                f"    {find.splitlines()[0]!r}\n"
                f"  (sokol layout changed -- update the script's anchors)")
        text = text.replace(find, repl, 1)
    return text


def _default_backend():
    if sys.platform.startswith("win"):
        return "SOKOL_D3D11"
    if sys.platform == "darwin":
        return "SOKOL_METAL"
    return "SOKOL_DUMMY_BACKEND"  # portable fallback (GL/WGPU need extra headers)


def verify(backend):
    """Compile a tiny TU (object-only, no link) that includes the patched sokol_gfx.h
    with `backend` and exercises the new context API. Returns 0 on success. This is the
    real acceptance test for the transform on a given backend."""
    cc = next((c for c in ("cc", "clang", "gcc", "cl") if shutil.which(c)), None)
    if cc is None:
        print("verify: no C compiler found (looked for cc/clang/gcc/cl)")
        return 1
    objc = backend == "SOKOL_METAL"  # Metal backend must compile as Objective-C
    tmp = tempfile.mkdtemp(prefix="sokol_verify_")
    try:
        src = os.path.join(tmp, "verify.m" if objc else "verify.c")
        obj = os.path.join(tmp, "verify.obj" if cc == "cl" else "verify.o")
        with open(src, "w", encoding="utf-8") as f:
            f.write(f'#define SOKOL_IMPL\n#define {backend}\n#include "sokol_gfx.h"\n'
                    "int main(void) {\n"
                    "    void* c = sg_make_context();\n"
                    "    sg_set_current_context(c);\n"
                    "    (void)sg_get_current_context();\n"
                    "    sg_destroy_context(c);\n"
                    "    return 0;\n}\n")
        if cc == "cl":
            cmd = [cc, "/nologo", "/c", "/I" + _SOKOL, src, "/Fo" + obj]
        else:
            cmd = [cc, "-c", "-Wall", "-I", _SOKOL]
            if objc:
                cmd += ["-x", "objective-c"]
            cmd += [src, "-o", obj]
        print(f"verify: {cc} -D{backend} (object-only) ...")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0:
            print(f"verify: {backend} compiles OK")
        else:
            print(f"verify: {backend} FAILED\n{r.stdout}{r.stderr}")
        return r.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="print diff, do not write")
    ap.add_argument("--verify", action="store_true",
                    help="after patching, compile a smoke-test TU for the platform "
                         "backend to confirm the transform builds")
    ap.add_argument("--backend", default=None, metavar="SOKOL_X",
                    help=f"backend define for --verify (default: {_default_backend()})")
    args = ap.parse_args()

    # sokol headers contain non-Latin1 bytes (e.g. accented names in comments); make
    # diff output robust on a Windows cp1252 console.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    any_changed = False
    for t in TARGETS:
        path = t["path"]
        if not os.path.isfile(path):
            raise SystemExit(f"not found: {path}\n(is sokol cloned in lib/sokol?)")
        with open(path, "r", encoding="utf-8", newline="") as f:
            original = f.read()

        new = apply_patch(original, t)
        if new == original:
            print(f"-- {os.path.basename(path)}: already up to date")
            continue

        diff = difflib.unified_diff(
            original.splitlines(keepends=True), new.splitlines(keepends=True),
            fromfile=f"a/{os.path.basename(path)}", tofile=f"b/{os.path.basename(path)}")
        sys.stdout.writelines(diff)

        if not args.dry_run:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            any_changed = True

    if args.dry_run:
        print("\n(dry run -- no files written)")
        return 0
    if any_changed:
        print("\nPatched.")

    if args.verify:
        return verify(args.backend or _default_backend())
    if any_changed:
        print("Verify a backend builds, e.g.:  "
              "python tools/patch_sokol_instanceable.py --verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
