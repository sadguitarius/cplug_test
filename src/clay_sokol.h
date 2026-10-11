// Clay renderer on sokol_gfx with no global state, so each plugin instance
// owns one. Every call needs that instance's sokol_gfx context current.
#ifndef CLAY_SOKOL_H
#define CLAY_SOKOL_H

#include <stdbool.h>
#include <stddef.h>

#include <clay.h>

typedef struct ClaySokol ClaySokol;

// Takes ownership of ttf (malloc'd), freed by clay_sokol_destroy.
// Pipeline formats follow the sg_setup() environment defaults.
ClaySokol *clay_sokol_create(unsigned char *ttf);
void clay_sokol_destroy(ClaySokol *cs);

// Pixels per Clay layout unit. Returns true if the scale changed; the caller
// then calls Clay_ResetMeasureTextCache() because text widths changed.
bool clay_sokol_set_scale(ClaySokol *cs, float scale);

// Pass to Clay_SetMeasureTextFunction() with the ClaySokol as userData.
Clay_Dimensions clay_sokol_measure_text(Clay_StringSlice text,
                                        Clay_TextElementConfig *config,
                                        void *userData);

// Draws a Clay custom element. box is in pixels. Draw into it with
// clay_sokol_line(); the element's clip rect is already applied.
typedef void (*ClaySokolCustomFn)(ClaySokol *cs, Clay_BoundingBox box,
                                  void *customData, void *user);

// Builds and uploads vertices. Call outside a render pass.
// custom may be NULL, which skips custom elements.
void clay_sokol_prepare(ClaySokol *cs, Clay_RenderCommandArray cmds, int fb_width,
                        int fb_height, ClaySokolCustomFn custom, void *user);

// Anti-aliased line with round caps, in pixels. Only valid inside a
// ClaySokolCustomFn.
void clay_sokol_line(ClaySokol *cs, float x0, float y0, float x1, float y1,
                     float width, Clay_Color color);

// A soft halo for a line drawn with the same ends and width: solid over the
// line itself, fading out over glow pixels around it. Draw it before the line.
// Only valid inside a ClaySokolCustomFn.
void clay_sokol_glow_line(ClaySokol *cs, float x0, float y0, float x1, float y1,
                          float width, float glow, Clay_Color color);

// Draws what clay_sokol_prepare built. Call inside a render pass.
void clay_sokol_draw(ClaySokol *cs);

#endif // CLAY_SOKOL_H
