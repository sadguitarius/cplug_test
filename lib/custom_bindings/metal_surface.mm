#import <MetalKit/MetalKit.h>

#include <stdlib.h>

#include <cplug_extensions/window.h>
#include "metal_surface.h"

typedef struct MetalSurface
{
    id<MTLDevice>               device;
    id<MTLCommandQueue>         queue;
    MTLRenderPassDescriptor*    rpd; // retained between begin_frame and end_render
    id<MTLCommandBuffer>        cmd; // valid only between begin_render and end_render
    id<MTLRenderCommandEncoder> enc; // valid only between begin_render and end_render
} MetalSurface;

void* metal_surface_init(void* pw)
{
    MetalSurface* ms   = (MetalSurface*)calloc(1, sizeof(*ms));
    MTKView*      view = (MTKView*)pw_get_native_window(pw);
    ms->device         = view.device;
    ms->queue          = [view.device newCommandQueue];
    return ms;
}

void metal_surface_shutdown(void* _ms)
{
    MetalSurface* ms = (MetalSurface*)_ms;
    if (ms == NULL)
        return;
    [ms->queue release];
    free(ms);
}

void* metal_surface_device(void* _ms)
{
    MetalSurface* ms = (MetalSurface*)_ms;
    return ms->device;
}

void* metal_surface_begin_frame(void* _ms, void* pw)
{
    MetalSurface*            ms   = (MetalSurface*)_ms;
    MTKView*                 view = (MTKView*)pw_get_native_window(pw);
    MTLRenderPassDescriptor* rpd  = view.currentRenderPassDescriptor;
    if (rpd == nil)
        return NULL;
    ms->rpd = [rpd retain];
    return rpd;
}

void* metal_surface_begin_render(void* _ms, const float clear_color[4], void** out_cmd)
{
    MetalSurface* ms = (MetalSurface*)_ms;
    if (ms->rpd == nil)
        return NULL;

    ms->rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    ms->rpd.colorAttachments[0].clearColor =
        MTLClearColorMake(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);

    ms->cmd = [ms->queue commandBuffer];
    ms->enc = [ms->cmd renderCommandEncoderWithDescriptor:ms->rpd];
    if (out_cmd)
        *out_cmd = ms->cmd;
    return ms->enc;
}

void metal_surface_end_render(void* _ms, void* pw)
{
    MetalSurface* ms   = (MetalSurface*)_ms;
    MTKView*      view = (MTKView*)pw_get_native_window(pw);
    if (ms->rpd == nil)
        return;

    [ms->enc endEncoding];
    [ms->cmd presentDrawable:view.currentDrawable];
    [ms->cmd commit];

    [ms->rpd release];
    ms->rpd = nil;
    ms->cmd = nil;
    ms->enc = nil;
}
