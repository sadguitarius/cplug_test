// The only translation unit that compiles the sokol implementation.
//
// compiled as Objective-C with ARC on Mac for Metal backend

// "static _sg_state_t _sg;" in sokol_gfx.h becomes a static pointer, and every
// _sg.field access goes through it; _simgui works the same way. These must be
// the libraries' only mutable statics: if this object file's .data/.bss gains
// symbols other than the pointers below, a new upstream static is escaping the
// redirect.
#define _sg     (*g_sg)
#define _simgui (*g_simgui)

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

#undef _sg
#undef _simgui

#include <stdlib.h>

#include <clay.h>
#include <cplug.h>

#include "gui_libs.h"

struct GuiLibs {
    _sg_state_t *sg;         // zeroed, like the static it replaces
    _simgui_state_t *simgui; // ditto
    ImGuiContext *imgui;     // NULL until simgui_setup() creates it
    Clay_Context *clay;
    void *clay_mem;
};

static void log_clay_error(Clay_ErrorData error) {
    cplug_log("clay: %.*s\n", (int)error.errorText.length, error.errorText.chars);
}

// The instance whose state is current. Each gui_libs_enter() first records
// the current ImGui context into it, which picks up the context that
// simgui_setup() created.
static GuiLibs *g_active;

GuiLibs *gui_libs_create(void) {
    uint32_t clay_size = Clay_MinMemorySize();
    GuiLibs *libs = calloc(1, sizeof(*libs));
    if (libs) {
        libs->sg = calloc(1, sizeof(_sg_state_t));
        libs->simgui = calloc(1, sizeof(_simgui_state_t));
        libs->clay_mem = malloc(clay_size);
    }
    if (!libs || !libs->sg || !libs->simgui || !libs->clay_mem) {
        if (libs) {
            free(libs->sg);
            free(libs->simgui);
            free(libs->clay_mem);
        }
        free(libs);
        return NULL;
    }
    libs->clay = Clay_Initialize(
        Clay_CreateArenaWithCapacityAndMemory(clay_size, libs->clay_mem),
        (Clay_Dimensions){1, 1}, (Clay_ErrorHandler){log_clay_error, NULL});
    return libs;
}

void gui_libs_destroy(GuiLibs *libs) {
    if (g_active == libs) {
        g_active = NULL;
        g_sg = NULL;
        g_simgui = NULL;
    }
    if (Clay_GetCurrentContext() == libs->clay)
        Clay_SetCurrentContext(NULL);
    // sg_shutdown() leaves some Objective-C references set (cur_drawable).
    // Under ARC this clear releases them, as sg_setup() does for the static.
    _SG_CLEAR_ARC_STRUCT(_sg_state_t, *libs->sg);
    free(libs->sg);
    free(libs->simgui);
    free(libs->clay_mem);
    free(libs);
}

void gui_libs_enter(GuiLibs *libs) {
    if (g_active)
        g_active->imgui = ImGui_GetCurrentContext();
    g_active = libs;
    g_sg = libs->sg;
    g_simgui = libs->simgui;
    ImGui_SetCurrentContext(libs->imgui);
    Clay_SetCurrentContext(libs->clay);
}
