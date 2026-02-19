#include "defs.h"
#include "imgui_internal.h"
// #include "imgui_internal.h"
#include <cassert>
#include <cplug.h>
#include <cplug_extensions/window.h>

// #include <cstdint>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <threads.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

// Data
struct ImGuiState {
    ImGuiContext *imgui_context;

    ImFont *font;

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    int counter = 0;
    float f = 0.0f;
    int mouse_x = 0;
    int mouse_y = 0;
    int mouse_button_pressed = 0;
};

void imgui_start(GUI *gui) {
    ImGuiState *state = (ImGuiState *)calloc(1, sizeof(*state));

    state->clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    gui->imgui_state = state;

    IMGUI_CHECKVERSION();

    auto *imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_context);
    state->imgui_context = imgui_context;

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    state->font = io.Fonts->AddFontFromMemoryCompressedTTF(
        Iosevka_compressed_data, Iosevka_compressed_size, 36);

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup scaling
    //
    float scale = pw_get_content_scale_factor(gui->pw);
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init((HWND)pw_get_native_window(gui->pw));
    ImGui_ImplDX11_Init(
        (ID3D11Device *)pw_get_dx11_device(gui->pw),
        (ID3D11DeviceContext *)pw_get_dx11_device_context(gui->pw));
}

void imgui_set_scale(GUI *gui, float scale) {
    ImGuiState *state = gui->imgui_state;
    CPLUG_LOG_ASSERT(state->imgui_context != NULL)
    ImGui::SetCurrentContext(state->imgui_context);

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

void imgui_stop(GUI *gui) {
    ImGuiState *state = gui->imgui_state;
    ImGui::SetCurrentContext(state->imgui_context);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    free(gui->imgui_state);
}

void imgui_tick(GUI *gui) {
    ImGuiState *state = gui->imgui_state;
    ImGui::SetCurrentContext(state->imgui_context);

    float scale = pw_get_content_scale_factor(gui->pw);
    float width = static_cast<float>(gui->normalized_width) * scale;
    float height = static_cast<float>(gui->normalized_height) * scale;

    // Start the Dear ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // cplug_log("g.FrameCount: %d, g.FrameCountEnded: %d",
    // state->imgui_context->FrameCount, state->imgui_context->FrameCountEnded);
    ImGui::NewFrame();
    ImGui::PushFont(state->font, 18);
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = {20.0f, 20.0f};
    style.FrameRounding = 10.0f;
    style.FramePadding = {10.0f, 5.0f};
    style.GrabRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.TabRounding = 10.0f;
    style.ChildRounding = 10.0f;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width, height});
    ImGui::Begin("Demo Plugin", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

    ImGui::Text("This is some useful text.");
    ImGui::SliderFloat("float", &state->f, 0.0f, 1.0f);
    ImGui::ColorEdit3("clear color", (float *)&gui->imgui_state->clear_color);

    if (ImGui::Button("Button"))
        state->counter++;

    ImGui::SameLine();
    ImGui::Text("counter = %d", state->counter);
    ImGui::Text("f = %f, mouse X = %4d, mouse Y = %4d, button = %d", state->f,
                state->mouse_x, state->mouse_y, state->mouse_button_pressed);

    ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);
    // ImGui::Text("width: %.3f, height: %.3f, scale: %.3f", width, height,
    // scale);
    //
    // size_t bufsize = sizeof(int16_t) * 2 *
    //                  128; // 128 character utf16 (character can be 2 units)
    // char *name_buf = (char *)PW_MALLOC(bufsize);
    // gui->plugin->hostContext->getHostName(gui->plugin->hostContext, name_buf,
    //                                       bufsize);
    // ImGui::Text("host name: %s", name_buf);
    // PW_FREE(name_buf);
    ImGui::End();
    ImGui::PopFont();

    // Rendering
    ImGui::Render();
    const float clear_color_with_alpha[4] = {
        state->clear_color.x * state->clear_color.w,
        state->clear_color.y * state->clear_color.w,
        state->clear_color.z * state->clear_color.w, state->clear_color.w};

    auto *context =
        ((ID3D11DeviceContext *)pw_get_dx11_device_context(gui->pw));
    auto *target_view =
        (ID3D11RenderTargetView *)pw_get_dx11_render_target_view(gui->pw);

    context->OMSetRenderTargets(
        1, &target_view,
        (ID3D11DepthStencilView *)pw_get_dx11_depth_stencil_view(gui->pw));
    context->ClearRenderTargetView(target_view, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void imgui_handle_event(GUI *gui, const PWEvent *event) {
    ImGuiState *state = gui->imgui_state;
    ImGui::SetCurrentContext(state->imgui_context);
    ImGuiIO &io = ImGui::GetIO();
    switch (event->type) {
    case PW_EVENT_MOUSE_MOVE:
        io.AddMousePosEvent(event->mouse.x, event->mouse.y);
        state->mouse_x = (int)event->mouse.x;
        state->mouse_y = (int)event->mouse.y;
        break;
    case PW_EVENT_MOUSE_LEFT_DOWN:
        io.AddMouseButtonEvent(0, true);
        state->mouse_button_pressed = 1;
        break;
    case PW_EVENT_MOUSE_LEFT_UP:
        io.AddMouseButtonEvent(0, false);
        state->mouse_button_pressed = 0;
        break;
    case PW_EVENT_MOUSE_RIGHT_DOWN:
        io.AddMouseButtonEvent(1, true);
        state->mouse_button_pressed = 1;
        break;
    case PW_EVENT_MOUSE_RIGHT_UP:
        io.AddMouseButtonEvent(1, false);
        state->mouse_button_pressed = 0;
        break;
    default:
        break;
    }
}
