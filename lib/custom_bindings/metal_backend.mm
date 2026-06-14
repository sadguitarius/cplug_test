#import <MetalKit/MetalKit.h>

#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_metal.h"

#include <cplug_extensions/window.h>
#include "metal_backend.h"

#ifndef IMGUI_DISABLE

typedef struct MetalBackend
{
    id<MTLCommandQueue>      queue;
    MTLRenderPassDescriptor* rpd;
} MetalBackend;

void* metal_backend_init(void* pw)
{
    MetalBackend* mb   = (MetalBackend*)calloc(1, sizeof(*mb));
    MTKView*      view = (MTKView*)pw_get_native_window(pw);
    mb->queue          = [view.device newCommandQueue];
    ImGui_ImplMetal_Init(view.device);
    return mb;
}

void metal_backend_shutdown(void* _mb)
{
    MetalBackend* mb = (MetalBackend*)_mb;
    if (mb == NULL)
        return;
    ImGui_ImplMetal_Shutdown();
    [mb->queue release];
    free(mb);
}

bool metal_backend_new_frame(void* _mb, void* pw)
{
    MetalBackend*            mb   = (MetalBackend*)_mb;
    MTKView*                 view = (MTKView*)pw_get_native_window(pw);
    MTLRenderPassDescriptor* rpd  = view.currentRenderPassDescriptor;
    if (rpd == nil)
        return false;
    mb->rpd = [rpd retain];
    ImGui_ImplMetal_NewFrame(rpd);
    return true;
}

void metal_backend_render(void* _mb, void* pw, void* draw_data, const float clear_color[4])
{
    MetalBackend* mb   = (MetalBackend*)_mb;
    MTKView*      view = (MTKView*)pw_get_native_window(pw);
    if (mb->rpd == nil)
        return;

    mb->rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    mb->rpd.colorAttachments[0].clearColor =
        MTLClearColorMake(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);

    id<MTLCommandBuffer>        cmd = [mb->queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:mb->rpd];
    ImGui_ImplMetal_RenderDrawData((ImDrawData*)draw_data, cmd, enc);
    [enc endEncoding];

    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];

    [mb->rpd release];
    mb->rpd = nil;
}

#endif // #ifndef IMGUI_DISABLE
