// Per-instance state for the GUI libraries, so each plugin instance's editor
// gets its own copy of libraries written as singletons.
//
// sokol_gfx and sokol_imgui keep all their state in one static struct each,
// _sg and _simgui. gui_libs.c compiles the unmodified upstream sources with
// both redirected through pointers, and gui_libs_enter() points them at one
// instance's state. Dear ImGui has its own context API, so its context
// switches here too.
//
// Call gui_libs_enter() first in every host callback that touches sokol or
// ImGui. All calls come from the host's UI thread, so editors never overlap.
#ifndef GUI_LIBS_H
#define GUI_LIBS_H

typedef struct GuiLibs GuiLibs;

// Returns NULL on allocation failure.
GuiLibs *gui_libs_create(void);

// Call after simgui_shutdown() and sg_shutdown() for this instance.
void gui_libs_destroy(GuiLibs *libs);

// simgui_setup() creates the instance's ImGui context and makes it current.
// The next gui_libs_enter() call records it for that instance.
void gui_libs_enter(GuiLibs *libs);

#endif // GUI_LIBS_H
