#include "defs.h"
#include "imgui_internal.h"
#include <cassert>
#include <cplug.h>
#include <cplug_extensions/window.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>
#include <threads.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

// Data
struct ImGuiState {
  HDC hDC;
  HGLRC hRC;

  // char uniqueClassName[64];

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

// Forward declarations of helper functions
bool CreateDeviceWGL(GUI *gui);
void CleanupDeviceWGL(GUI *gui);
void ResetDeviceWGL();

void imgui_init(GUI *gui) { ; }

void imgui_start(GUI *gui) {
  ImGuiState *state = (ImGuiState *)calloc(1, sizeof(*state));

  // LARGE_INTEGER timenow;
  // QueryPerformanceCounter(&timenow);
  // sprintf_s(state->uniqueClassName, sizeof(state->uniqueClassName),
  // "%s-%llx",
  //           CPLUG_PLUGIN_NAME, timenow.QuadPart);

  state->clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  gui->imgui_state = state;

  // Initialize OpenGL
  if (!CreateDeviceWGL(gui)) {
    CleanupDeviceWGL(gui);
    return;
  }
  wglMakeCurrent(state->hDC, state->hRC);

  IMGUI_CHECKVERSION();

  auto *imgui_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_context);
  state->imgui_context = imgui_context;

  ImGuiIO &io = ImGui::GetIO();
  state->font = io.Fonts->AddFontFromMemoryCompressedTTF(
      Iosevka_compressed_data, Iosevka_compressed_size, 36);

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  // Setup Platform/Renderer backends
  ImGui_ImplWin32_InitForOpenGL((HWND)pw_get_native_window(gui->pw));
  ImGui_ImplOpenGL3_Init();
}

void imgui_deinit(GUI *gui) {
  ImGuiState *state = gui->imgui_state;
  wglMakeCurrent(state->hDC, state->hRC);
  ImGui::SetCurrentContext(state->imgui_context);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  CleanupDeviceWGL(gui);
  wglDeleteContext(state->hRC);
  gui->imgui_state->hRC = nullptr;

  free(gui->imgui_state);
}

void imgui_tick(GUI *gui) {
  ImGuiState *state = gui->imgui_state;
  wglMakeCurrent(state->hDC, state->hRC);
  ImGui::SetCurrentContext(state->imgui_context);

  // Start the Dear ImGui frame
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  ImGui::PushFont(state->font, 36.0f);
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
  ImGui::SetNextWindowSize(
      {static_cast<float>(gui->width), static_cast<float>(gui->height)});
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
  ImGui::End();
  ImGui::PopFont();

  // Rendering
  ImGui::Render();
  glViewport(0, 0, gui->width, gui->height);
  glClearColor(state->clear_color.x, state->clear_color.y, state->clear_color.z,
               state->clear_color.w);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  // Present
  ::SwapBuffers(gui->imgui_state->hDC);
}

// Helper functions
bool CreateDeviceWGL(GUI *gui) {
  ImGuiState *state = gui->imgui_state;
  HWND hWnd = (HWND)pw_get_native_window(gui->pw);
  HDC hDc = ::GetDC(hWnd);
  PIXELFORMATDESCRIPTOR pfd = {0};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;

  const int pf = ::ChoosePixelFormat(hDc, &pfd);
  if (pf == 0)
    return false;
  if (::SetPixelFormat(hDc, pf, &pfd) == FALSE)
    return false;
  ::ReleaseDC(hWnd, hDc);

  state->hDC = ::GetDC(hWnd);
  if (!state->hRC)
    state->hRC = wglCreateContext(state->hDC);
  return true;
}

void CleanupDeviceWGL(GUI *gui) {
  wglMakeCurrent(nullptr, nullptr);
  ::ReleaseDC((HWND)pw_get_native_window(gui->pw), gui->imgui_state->hDC);
}

void imgui_handle_event(GUI *gui, const PWEvent *event) {
  ImGuiState *state = gui->imgui_state;
  ImGui::SetCurrentContext(state->imgui_context);
  ImGuiIO &io = ImGui::GetIO();
  switch (event->type) {
  case PW_EVENT_RESIZE_END:
    // imgui_deinit(gui);
    // imgui_init(gui);
    break;
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
