#!/usr/bin/env python3
"""Make sokol_gfx.h / sokol_imgui.h instanceable via an ambient "current context".

Rather than rewriting the ~thousands of `_sg.` / `_simgui.` member accesses (risky,
and a regex would mangle `&_sg`, `sizeof(_simgui)`, strings & comments), this turns
the single global STATE VALUE into a global STATE POINTER and defines the old name as
a macro that dereferences it:

    static _sg_state_t  _sg;            ->   static _sg_state_t* _sg_current;
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

The patch is two anchored edits per file (a prototype block + the declaration swap),
keyed on stable source strings (not line numbers) and guarded by sentinels so it is
idempotent and re-runnable after a sokol update. The C compiler is the acceptance
test: build each backend (-DSOKOL_METAL, -DSOKOL_D3D11, ...) and a green build means
the transform landed.

CAVEATS (see the chat discussion):
  * By default `_sg_current` is a shared mutable global -> not safe if instances
    render on DIFFERENT threads concurrently. For plugin UIs on the host's main
    thread this is fine. Pass --thread-local to make the pointer thread-local instead.
  * The per-context struct is allocated with calloc/free, NOT sokol's per-context
    sg_allocator (which lives *inside* the struct -- chicken/egg). Swap in your own
    allocator in sg_make_context/sg_destroy_context if you need to.
  * sokol_imgui.h's `_simgui` is a separate global and gets the same treatment here.
    Any other util header you adopt (sokol_gl's `_sgl`, etc.) needs its own pass.

Usage:
    python tools/patch_sokol_instanceable.py               # apply + print diff
    python tools/patch_sokol_instanceable.py --dry-run     # print diff only, no write
    python tools/patch_sokol_instanceable.py --thread-local  # thread-local context ptr
    python tools/patch_sokol_instanceable.py --verify      # apply, then compile-check
    python tools/patch_sokol_instanceable.py --revert      # strip the patch blocks
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

SENTINEL = "instanceable context patch"


def _block(tag, body):
    return f"/* >>> {SENTINEL} ({tag}) */\n{body}/* <<< {SENTINEL} ({tag}) */\n"


# Portable thread-local qualifier, emitted (guarded) only with --thread-local. Lets
# each *thread* hold its own current-context pointer, so instances that render on
# different threads don't race on it. Identical redefinition across both headers is
# fine; the #ifndef guard skips the second.
_TLS_MACRO = (
    "#ifndef SOKOL_INSTANCE_THREADLOCAL\n"
    "  #if defined(_MSC_VER)\n"
    "    #define SOKOL_INSTANCE_THREADLOCAL __declspec(thread)\n"
    "  #else\n"
    "    #define SOKOL_INSTANCE_THREADLOCAL _Thread_local\n"
    "  #endif\n"
    "#endif\n"
)

# Per-file recipe. `proto_after` and `state_decl` are exact source strings we anchor
# on. The prototype + impl blocks are generated from the remaining fields so the
# impl can vary (e.g. --thread-local).
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
    },
]


def _proto_block(t):
    a, p = t["api_decl"], t["prefix"]
    return _block(
        "proto",
        f"{a} void* {p}_make_context(void);\n"
        f"{a} void  {p}_set_current_context(void* ctx);\n"
        f"{a} void* {p}_get_current_context(void);\n"
        f"{a} void  {p}_destroy_context(void* ctx);\n",
    )


def _impl_block(t, thread_local):
    p, st = t["prefix"], t["state_type"]
    mac, ptr = t["macro_name"], t["ptr_name"]
    qual = "SOKOL_INSTANCE_THREADLOCAL " if thread_local else ""
    body = "#include <stdlib.h>\n"
    if thread_local:
        body += _TLS_MACRO
    body += (
        f"static {qual}{st}* {ptr};\n"
        f"#define {mac} (*{ptr})\n"
        f"SOKOL_API_IMPL void* {p}_make_context(void) {{ return calloc(1, sizeof({st})); }}\n"
        f"SOKOL_API_IMPL void  {p}_set_current_context(void* ctx) {{ {ptr} = ({st}*)ctx; }}\n"
        f"SOKOL_API_IMPL void* {p}_get_current_context(void) {{ return {ptr}; }}\n"
        f"SOKOL_API_IMPL void  {p}_destroy_context(void* ctx) {{ free(ctx); }}\n"
    )
    return _block("impl", body)


def _detect_nl(text):
    """The file's dominant line ending. sokol checked out on Windows is CRLF; our
    block templates are authored with '\\n', so we translate them to match and keep
    the round-trip byte-exact."""
    return "\r\n" if "\r\n" in text else "\n"


def _replace_block(text, tag, replacement, nl, consume_leading=False):
    """Replace a single `>>> ... (tag) */ ... /* <<< ... (tag) */` block with
    `replacement`, consuming the block's trailing newline (and, if `consume_leading`,
    one preceding newline too). The two flags make revert an exact inverse of apply:
    the proto block was inserted as `nl + block` after its anchor (consume both
    sides), while the impl block replaced a bare declaration (consume trailing only).
    Returns text unchanged if the block is absent."""
    open_marker = f"/* >>> {SENTINEL} ({tag}) */"
    close_marker = f"/* <<< {SENTINEL} ({tag}) */"
    start = text.find(open_marker)
    if start == -1:
        return text
    end = text.find(close_marker, start)
    if end == -1:
        raise SystemExit(f"unterminated patch block ({tag}) -- edit the file by hand")
    end += len(close_marker)
    if text[end:end + len(nl)] == nl:
        end += len(nl)
    if consume_leading and text[start - len(nl):start] == nl:
        start -= len(nl)
    return text[:start] + replacement + text[end:]


def apply_patch(text, t, thread_local=False):
    if t["proto_after"] not in text:
        raise SystemExit(f"  anchor not found: {t['proto_after']!r}\n"
                         f"  (sokol layout changed -- update the script's anchors)")
    if t["state_decl"] not in text:
        raise SystemExit(f"  anchor not found: {t['state_decl']!r}")
    nl = _detect_nl(text)
    proto = _proto_block(t).replace("\n", nl)
    impl = _impl_block(t, thread_local).replace("\n", nl)
    # Insert prototypes right after the shutdown declaration line.
    text = text.replace(t["proto_after"], t["proto_after"] + nl + proto, 1)
    # Swap the global value declaration for pointer + macro + context API.
    text = text.replace(t["state_decl"], impl, 1)
    return text


def revert_patch(text, t):
    # Drop the prototype block entirely; restore the impl block to the original decl.
    # The extra blank line we inserted after the prototype anchor is removed too.
    nl = _detect_nl(text)
    text = _replace_block(text, "proto", "", nl, consume_leading=True)
    text = _replace_block(text, "impl", t["state_decl"], nl)
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
            print(f"verify: {backend} compiles OK ✓")
        else:
            print(f"verify: {backend} FAILED ✗\n{r.stdout}{r.stderr}")
        return r.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="print diff, do not write")
    ap.add_argument("--revert", action="store_true", help="strip the patch")
    ap.add_argument("--thread-local", action="store_true", dest="thread_local",
                    help="make the current-context pointer thread-local (safe when "
                         "instances render on different threads)")
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

        patched = SENTINEL in original
        if args.revert:
            if not patched:
                print(f"-- {os.path.basename(path)}: not patched, nothing to revert")
                continue
            new = revert_patch(original, t)
        else:
            if patched:
                print(f"-- {os.path.basename(path)}: already patched, skipping")
                continue
            new = apply_patch(original, t, thread_local=args.thread_local)

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
        tls = " (thread-local)" if args.thread_local else ""
        print(f"\nPatched{tls}.")

    if args.verify:
        return verify(args.backend or _default_backend())
    if any_changed:
        print("Verify a backend builds, e.g.:  "
              "python tools/patch_sokol_instanceable.py --verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
