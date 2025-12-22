#ifndef DEFS_H
#define DEFS_H

// #include <cstdint>
#ifdef __cplusplus
extern "C" {
#endif

#include <cplug.h>
#include <cplug_extensions/window.h>

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

static const uint32_t PARAM_IDS[] = {
    'pf32',
    'pi32',
    'bool',
    'utf8',
};
enum { NUM_PARAMS = ARRLEN(PARAM_IDS) };

typedef struct ParamInfo {
  float min;
  float max;
  float defaultValue;
  int flags;
} ParamInfo;

typedef struct Plugin {
  CplugHostContext *hostContext;

  ParamInfo paramInfo[NUM_PARAMS];

  float sampleRate;
  uint32_t maxBufferSize;

  float paramValuesAudio[NUM_PARAMS];

  float oscPhase; // 0-1
  int midiNote;   // -1 == not playing, 0-127+ playing
  float velocity; // 0-1

  // GUI zone
  // void* gui;
  struct GUI *gui;
  uint32_t width;
  uint32_t height;

  float paramValuesMain[NUM_PARAMS];

  // Single reader writer queue. Pretty sure atomics aren't required, but here
  // anyway
  cplug_atomic_i32 mainToAudioHead;
  cplug_atomic_i32 mainToAudioTail;
  CplugEvent mainToAudioQueue[CPLUG_EVENT_QUEUE_SIZE];

  cplug_atomic_i32 audioToMainHead;
  cplug_atomic_i32 audioToMainTail;
  CplugEvent audioToMainQueue[CPLUG_EVENT_QUEUE_SIZE];

} Plugin;

typedef struct ImGuiState ImGuiState;

typedef struct GUI {

  Plugin *plugin;
  // void*     window; // HWND / NSView
  void *pw;
#ifdef _WIN32
  char uniqueClassName[64];
#endif

  uint32_t *img;
  float scale;

  bool mouseDragging;
  uint32_t dragParamId;
  int dragStartX;
  int dragStartY;
  double dragStartParamNormalised;
  double dragCurrentParamNormalised;

  ImGuiState *imgui_state;
} GUI;

void imgui_init(GUI *gui);
void imgui_deinit(GUI *gui);
void imgui_start(GUI *gui);
void imgui_tick(GUI *gui);
void imgui_handle_event(GUI *gui, const PWEvent *event);

extern const unsigned int Iosevka_compressed_size;
extern const unsigned char Iosevka_compressed_data[3753249];

#ifdef __cplusplus
}
#endif

#endif // DEFS_H
