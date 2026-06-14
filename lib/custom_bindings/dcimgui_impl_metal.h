#ifndef DCIMGUI_IMPL_METAL_H
#define DCIMGUI_IMPL_METAL_H

#ifdef __cplusplus
extern "C" {
#endif
void cImGui_ImplMetal_Init(void* device);
void cImGui_ImplMetal_Shutdown(void);
void cImGui_ImplMetal_NewFrame(void* rpd);
void cImGui_ImplMetal_Render(void* draw_data, void* cmd, void* enc);

#ifdef __cplusplus
}
#endif

#endif // DCIMGUI_IMPL_METAL_H
