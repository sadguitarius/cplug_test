#ifndef METAL_SURFACE_H
#define METAL_SURFACE_H

#ifdef __cplusplus
extern "C" {
#endif

void* metal_surface_init(void* pw);
void  metal_surface_shutdown(void* surface);

// id<MTLDevice> as void*, e.g. to hand to a renderer's init.
void* metal_surface_device(void* surface);

// New-frame phase: acquire and retain the view's current render pass descriptor.
// Returns the MTLRenderPassDescriptor* (as void*), or NULL if not ready this
// frame (caller should skip rendering). Must be paired with metal_surface_end_render.
void* metal_surface_begin_frame(void* surface, void* pw);

// Render phase: apply clear color, create a command buffer + render encoder.
// Returns the MTLRenderCommandEncoder* (as void*) to record draw commands into,
// and writes the MTLCommandBuffer* to *out_cmd. Returns NULL if no frame is active.
void* metal_surface_begin_render(void* surface, const float clear_color[4], void** out_cmd);

// End the render phase: end encoding, present the drawable, commit, release the rpd.
void metal_surface_end_render(void* surface, void* pw);

#ifdef __cplusplus
}
#endif

#endif // METAL_SURFACE_H
