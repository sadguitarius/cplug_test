#ifndef METAL_BACKEND_H
#define METAL_BACKEND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void* metal_backend_init(void* pw);
void  metal_backend_shutdown(void* backend);

bool metal_backend_new_frame(void* backend, void* pw);

void metal_backend_render(void* backend, void* pw, void* draw_data, const float clear_color[4]);

#ifdef __cplusplus
}
#endif

#endif // METAL_BACKEND_H
