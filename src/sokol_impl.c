// sokol implementation translation unit
//
// compiled as Objective-C with ARC on Mac for Metal backend

#define SOKOL_IMPL
#if defined(_WIN32)
#define SOKOL_D3D11
#elif defined(__APPLE__)
#define SOKOL_METAL
#endif

#include "sokol_gfx.h"
#include "sokol_log.h"

#define SOKOL_IMGUI_NO_SOKOL_APP
#define SOKOL_IMGUI_CPREFIX ImGui_
#include <dcimgui.h>
#include "util/sokol_imgui.h"
