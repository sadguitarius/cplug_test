#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_metal.h"

#include "dcimgui_impl_metal.h"

#ifndef IMGUI_DISABLE

void cImGui_ImplMetal_Init(void* device)
{
    ImGui_ImplMetal_Init((id<MTLDevice>)device);
}

void cImGui_ImplMetal_Shutdown(void)
{
    ImGui_ImplMetal_Shutdown();
}

void cImGui_ImplMetal_NewFrame(void* rpd)
{
    ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)rpd);
}

void cImGui_ImplMetal_Render(void* draw_data, void* cmd, void* enc)
{
    ImGui_ImplMetal_RenderDrawData(
        (ImDrawData*)draw_data, (id<MTLCommandBuffer>)cmd, (id<MTLRenderCommandEncoder>)enc);
}

#endif // #ifndef IMGUI_DISABLE
