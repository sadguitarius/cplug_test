// sokol implementation translation unit.
//
// This is the ONE place the sokol_gfx + sokol_imgui implementations are compiled.
// On macOS it must be built as Objective-C with ARC (the Metal backend uses ObjC);
// see CMakeLists.txt which sets its LANGUAGE/flags accordingly.
//
// sokol_gfx.h / sokol_imgui.h here are the *instanceable* (per-context) variants
// produced by tools/patch_sokol_instanceable.py.

#define SOKOL_IMPL
#if defined(_WIN32)
#define SOKOL_D3D11
#elif defined(__APPLE__)
#define SOKOL_METAL
#endif

#include "sokol_gfx.h"
#include "sokol_log.h" // slog_func: routes sokol validation/errors to the debugger

// sokol_imgui renders Dear ImGui draw data through sokol_gfx (replacing the old
// imgui_impl_metal / imgui_impl_dx11 renderer backends).
//   - We drive the window/events through CPLUG, not sokol_app, so compile out the
//     sokol_app glue inside sokol_imgui (simgui_new_frame etc. call sapp_* otherwise).
//   - Bind sokol_imgui's internal Dear ImGui calls to our dcimgui (ImGui_*) bindings
//     instead of the default cimgui (ig*) ones.
#define SOKOL_IMGUI_NO_SOKOL_APP
#define SOKOL_IMGUI_CPREFIX ImGui_
#include <dcimgui.h>
#include "util/sokol_imgui.h"

#if defined(__APPLE__)
// macOS-only helper (NOT sokol): pw_tick is driven by a CFRunLoopTimer, so it can
// fire before the MTKView has a drawable. pw_get_metal_drawable() PW_ASSERTs on nil,
// so main.c uses this to skip such frames. Lives here because this TU is already
// compiled as Objective-C on macOS.
#import <MetalKit/MetalKit.h>
#include <cplug_extensions/window.h>

bool pw_metal_drawable_ready(void *pw)
{
    MTKView *view = (__bridge MTKView *)pw_get_native_window(pw);
    return view.currentDrawable != nil;
}
#endif // __APPLE__
