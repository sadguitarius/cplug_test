#include <cplug.h>
#include <cplug_extensions/window.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <whereami.h>

#include <dcimgui.h>

#include <sokol_gfx.h>
#include <sokol_log.h>
#define SOKOL_IMGUI_NO_SOKOL_APP
#include <sokol_imgui.h>

// #ifndef _WIN32
// // Implemented in sokol_impl.c (Objective-C). True when the Metal drawable is ready
// // this frame; pw_tick runs on a timer that can fire before it is.
// bool pw_metal_drawable_ready(void *pw);
// #endif

#ifdef _WIN32
#include <ShellScalingApi.h>

#define my_assert(cond) (cond) ? (void)0 : __debugbreak()
#else
#define my_assert(cond) (cond) ? (void)0 : __builtin_debugtrap()
#endif // _WIN32

// #if defined(_WIN32) && defined(__x86_64__)
// https://softwareengineering.stackexchange.com/a/337251
#include <immintrin.h>
#define DISABLE_DENORMALS                                                      \
    unsigned int oldMXCSR = _mm_getcsr(); /*read the old MXCSR setting  */     \
    unsigned int newMXCSR = oldMXCSR |= 0x8040; /* set DAZ and FZ bits */      \
    _mm_setcsr(newMXCSR); /* write the new MXCSR setting to the MXCSR */
#define RESTORE_DENORMALS _mm_setcsr(oldMXCSR);
// #else
// #include <fenv.h>
// #if defined(__x86_64__)
// #define DISABLE_DENORMS_ENV &_FE_DFL_DISABLE_SSE_DENORMS_ENV
// #elif defined(__arm64__)
// #define DISABLE_DENORMS_ENV &_FE_DFL_DISABLE_DENORMS_ENV
// #endif // x84, ARM64
//
// #define DISABLE_DENORMALS \
//   fenv_t _fenv; \
//   fegetenv(&_fenv); \ fesetenv(DISABLE_DENORMS_ENV);
// #define RESTORE_DENORMALS fesetenv(&_fenv);
// #endif

#define ARRLEN(a)              (sizeof(a) / sizeof((a)[0]))
#define CPLUG_EVENT_QUEUE_MASK (CPLUG_EVENT_QUEUE_SIZE - 1)

static const uint32_t PARAM_IDS[] = {
    'pf32',
    'pi32',
    'bool',
    'utf8',
};
enum { NUM_PARAMS = ARRLEN(PARAM_IDS) };

#define GUI_DEFAULT_WIDTH  512
#define GUI_DEFAULT_HEIGHT 250

// returns 'CPLUG_NUM_PARAMS' on failure
uint32_t get_param_index(void *ptr, uint32_t paramId) {
    uint32_t i = 0;
    for (; i < ARRLEN(PARAM_IDS); i++)
        if (paramId == PARAM_IDS[i])
            break;
    return i;
}

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
    struct GUI *gui;

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

void sendParamEventFromMain(Plugin *plugin, uint32_t type, uint32_t paramIdx,
                            double value);

typedef struct GUI {
    Plugin *plugin;
    void *pw;
#ifdef _WIN32
    char uniqueClassName[64];
#endif

    uint32_t normalized_width;
    uint32_t normalized_height;

    bool mouseDragging;
    uint32_t dragParamId;
    int dragStartX;
    int dragStartY;
    double dragStartParamNormalised;
    double dragCurrentParamNormalised;

    ImGuiContext *imgui_ctx;

    void *sokol_gfx_ctx;
    void *sokol_imgui_ctx;

    ImFont *font;

    // Our state
    ImVec4 clear_color;
    int counter;
    float f;
    int mouse_x;
    int mouse_y;
    int mouse_button_pressed;
} GUI;

void cplug_libraryLoad() {};
void cplug_libraryUnload() {};

void *cplug_createPlugin(CplugHostContext *ctx) {
    Plugin *plugin = (Plugin *)calloc(1, sizeof(Plugin));
    plugin->hostContext = ctx;

    uint32_t idx;
    // Init params
    // 'pf32'
    idx = get_param_index(plugin, 'pf32');
    plugin->paramInfo[idx].flags = CPLUG_FLAG_PARAMETER_IS_AUTOMATABLE;
    plugin->paramInfo[idx].max = 100.0f;
    plugin->paramInfo[idx].defaultValue = 50.0f;

    // 'pi32'
    idx = get_param_index(plugin, 'pi32');
    plugin->paramValuesAudio[idx] = 2.0f;
    plugin->paramInfo[idx].flags =
        CPLUG_FLAG_PARAMETER_IS_AUTOMATABLE | CPLUG_FLAG_PARAMETER_IS_INTEGER;
    plugin->paramInfo[idx].min = 2.0f;
    plugin->paramInfo[idx].max = 5.0f;
    plugin->paramInfo[idx].defaultValue = 2.0f;

    // 'bool'
    idx = get_param_index(plugin, 'bool');
    plugin->paramValuesAudio[idx] = 0.0f;
    plugin->paramInfo[idx].flags = CPLUG_FLAG_PARAMETER_IS_BOOL;
    plugin->paramInfo[idx].max = 1.0f;

    // 'utf8'
    idx = get_param_index(plugin, 'utf8');
    plugin->paramValuesAudio[idx] = 0.0f;
    plugin->paramInfo[idx].flags = CPLUG_FLAG_PARAMETER_IS_AUTOMATABLE;
    plugin->paramInfo[idx].min = 0.0f;
    plugin->paramInfo[idx].max = 1.0f;
    plugin->paramInfo[idx].defaultValue = 0.0f;

    plugin->midiNote = -1;

    return plugin;
}
void cplug_destroyPlugin(void *ptr) {
    // Free any allocated resources in your plugin here
    free(ptr);
}

/* --------------------------------------------------------------------------------------------------------
 * Busses */

uint32_t cplug_getNumInputBusses(void *ptr) { return 1; }
uint32_t cplug_getNumOutputBusses(void *ptr) { return 1; }
uint32_t cplug_getInputBusChannelCount(void *ptr, uint32_t idx) { return 2; }
uint32_t cplug_getOutputBusChannelCount(void *ptr, uint32_t idx) { return 2; }

void cplug_getInputBusName(void *ptr, uint32_t idx, char *buf, size_t buflen) {
    snprintf(buf, buflen, "Stereo Input");
}

void cplug_getOutputBusName(void *ptr, uint32_t idx, char *buf, size_t buflen) {
    snprintf(buf, buflen, "Stereo Output");
}

/* --------------------------------------------------------------------------------------------------------
 * Parameters */

uint32_t cplug_getNumParameters(void *ptr) { return ARRLEN(PARAM_IDS); }
uint32_t cplug_getParameterID(void *ptr, uint32_t paramIndex) {
    return PARAM_IDS[paramIndex];
}

void cplug_getParameterName(void *ptr, uint32_t paramId, char *buf,
                            size_t buflen) {
    static const char *param_names[] = {"Parameter Float", "Parameter Int",
                                        "Parameter Bool",
                                        // https://utf8everywhere.org/
                                        // UTF8    = 1 byte per character
                                        // Приве́т  = 2 bytes
                                        // नमस्ते     = 3 bytes
                                        // שלום = 3 בייטים
                                        // 🐨       = 4 bytes
                                        "UTF8 Приве́т नमस्ते שָׁלוֹם 🐨"};
    // static_assert(ARRLEN(param_names) == ARRLEN(PARAM_IDS), "Invalid
    // length");

    uint32_t index = get_param_index(ptr, paramId);
    snprintf(buf, buflen, "%s", param_names[index]);
}

double cplug_getParameterValue(void *ptr, uint32_t paramId) {
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    double val = plugin->paramValuesAudio[index];
    if (plugin->paramInfo[index].flags & CPLUG_FLAG_PARAMETER_IS_INTEGER)
        val = round(val);
    return val;
}

double cplug_getDefaultParameterValue(void *ptr, uint32_t paramId) {
    Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);
    return plugin->paramInfo[index].defaultValue;
}

void cplug_setParameterValue(void *ptr, uint32_t paramId, double value) {
    Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    ParamInfo *info = &plugin->paramInfo[index];
    if (value < info->min)
        value = info->min;
    if (value > info->max)
        value = info->max;
    plugin->paramValuesAudio[index] = (float)value;

    // Send incoming param update to GUI
    if (plugin->gui) {
        int queueWritePos = cplug_atomic_load_i32(&plugin->audioToMainHead) &
                            CPLUG_EVENT_QUEUE_MASK;

        plugin->audioToMainQueue[queueWritePos].parameter.type =
            CPLUG_EVENT_PARAM_CHANGE_UPDATE;
        plugin->audioToMainQueue[queueWritePos].parameter.id = paramId;
        plugin->audioToMainQueue[queueWritePos].parameter.value = value;

        cplug_atomic_fetch_add_i32(&plugin->audioToMainHead, 1);
        cplug_atomic_fetch_and_i32(&plugin->audioToMainHead,
                                   CPLUG_EVENT_QUEUE_MASK);
    }
}

double cplug_denormaliseParameterValue(void *ptr, uint32_t paramId,
                                       double normalised) {
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    const ParamInfo *info = &plugin->paramInfo[index];

    double denormalised = normalised * (info->max - info->min) + info->min;

    if (denormalised < info->min)
        denormalised = info->min;
    if (denormalised > info->max)
        denormalised = info->max;
    return denormalised;
}

double cplug_normaliseParameterValue(void *ptr, uint32_t paramId,
                                     double denormalised) {
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    const ParamInfo *info = &plugin->paramInfo[index];

    // If this fails, your param range is likely not initialised, causing a
    // division by zero and producing infinity
    double normalised = (denormalised - info->min) / (info->max - info->min);
    my_assert(normalised == normalised);

    if (normalised < 0.0f)
        normalised = 0.0f;
    if (normalised > 1.0f)
        normalised = 1.0f;
    return normalised;
}

double cplug_parameterStringToValue(void *ptr, uint32_t paramId,
                                    const char *str) {
    double value;
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    const unsigned flags = plugin->paramInfo[index].flags;

    if (flags & CPLUG_FLAG_PARAMETER_IS_INTEGER)
        value = (double)atoi(str);
    else
        value = atof(str);

    return value;
}

void cplug_parameterValueToString(void *ptr, uint32_t paramId, char *buf,
                                  size_t bufsize, double value) {
    const Plugin *plugin = (Plugin *)ptr;

    uint32_t index = get_param_index(ptr, paramId);

    const uint32_t flags = plugin->paramInfo[index].flags;

    if (flags & CPLUG_FLAG_PARAMETER_IS_BOOL)
        value = value >= 0.5 ? 1 : 0;

    if (paramId == 'utf8')
        snprintf(buf, bufsize, "%.2f Приве́т नमस्ते שָׁלוֹם 🐨", value);
    else if (flags &
             (CPLUG_FLAG_PARAMETER_IS_INTEGER | CPLUG_FLAG_PARAMETER_IS_BOOL))
        snprintf(buf, bufsize, "%d", (int)value);
    else
        snprintf(buf, bufsize, "%.2f", value);
}

void cplug_getParameterRange(void *ptr, uint32_t paramId, double *min,
                             double *max) {
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);

    *min = plugin->paramInfo[index].min;
    *max = plugin->paramInfo[index].max;
}

uint32_t cplug_getParameterFlags(void *ptr, uint32_t paramId) {
    const Plugin *plugin = (Plugin *)ptr;
    uint32_t index = get_param_index(ptr, paramId);
    return plugin->paramInfo[index].flags;
}

/* --------------------------------------------------------------------------------------------------------
 * Audio/MIDI Processing */

uint32_t cplug_getLatencyInSamples(void *ptr) { return 0; }
uint32_t cplug_getTailInSamples(void *ptr) { return 0; }

void cplug_setSampleRateAndBlockSize(void *ptr, double sampleRate,
                                     uint32_t maxBlockSize) {
    Plugin *plugin = (Plugin *)ptr;
    plugin->sampleRate = (float)sampleRate;
    plugin->maxBufferSize = maxBlockSize;
}

void cplug_process(void *ptr, CplugProcessContext *ctx) {
    DISABLE_DENORMALS

    Plugin *plugin = (Plugin *)ptr;

    // Audio thread has chance to respond to incoming GUI events before being
    // sent to the host
    int head = cplug_atomic_load_i32(&plugin->mainToAudioHead) &
               CPLUG_EVENT_QUEUE_MASK;
    int tail = cplug_atomic_load_i32(&plugin->mainToAudioTail);

    while (tail != head) {
        CplugEvent *event = &plugin->mainToAudioQueue[tail];

        if (event->type == CPLUG_EVENT_PARAM_CHANGE_UPDATE) {
            uint32_t idx = get_param_index(ptr, event->parameter.id);
            plugin->paramValuesAudio[idx] = (float)event->parameter.value;
        }

        ctx->enqueueEvent(ctx, event, 0);

        tail++;
        tail &= CPLUG_EVENT_QUEUE_MASK;
    }
    cplug_atomic_exchange_i32(&plugin->mainToAudioTail, tail);

    // "Sample accurate" process loop
    CplugEvent event;
    uint32_t frame = 0;
    while (ctx->dequeueEvent(ctx, &event, frame)) {
        switch (event.type) {
        case CPLUG_EVENT_UNHANDLED_EVENT:
            break;
        case CPLUG_EVENT_PARAM_CHANGE_UPDATE: {
            cplug_setParameterValue(plugin, event.parameter.id,
                                    event.parameter.value);
            break;
        }
        case CPLUG_EVENT_MIDI: {
            static const uint8_t MIDI_NOTE_OFF = 0x80;
            static const uint8_t MIDI_NOTE_ON = 0x90;
            static const uint8_t MIDI_NOTE_PITCH_WHEEL = 0xe0;

            if ((event.midi.status & 0xf0) == MIDI_NOTE_ON) {
                plugin->midiNote = event.midi.data1;
                plugin->velocity = (float)event.midi.data2 / 127.0f;
            }
            if ((event.midi.status & 0xf0) == MIDI_NOTE_OFF) {
                int note = event.midi.data1;
                if (note == plugin->midiNote)
                    plugin->midiNote = -1;
                plugin->velocity = (float)event.midi.data2 / 127.0f;
            }
            if ((event.midi.status & 0xf0) == MIDI_NOTE_PITCH_WHEEL) {
                // int pb = (int)event.midi.data1 | ((int)event.midi.data2 <<
                // 7);
            }
            break;
        }
        case CPLUG_EVENT_PROCESS_AUDIO: {
            // If your plugin does not require sample accurate processing, use
            // this line below to break the loop frame =
            // event.processAudio.endFrame;

            float **output = ctx->getAudioOutput(ctx, 0);
            CPLUG_LOG_ASSERT(output != NULL)
            CPLUG_LOG_ASSERT(output[0] != NULL);
            CPLUG_LOG_ASSERT(output[1] != NULL);

            if (plugin->midiNote == -1) {
                // Silence
                memset(&output[0][frame], 0,
                       sizeof(float) * (event.processAudio.endFrame - frame));
                memset(&output[1][frame], 0,
                       sizeof(float) * (event.processAudio.endFrame - frame));
                frame = event.processAudio.endFrame;
            } else {
                float phase = plugin->oscPhase;

                float Hz = 440.0f * exp2f(((float)plugin->midiNote - 69.0f) *
                                          0.0833333f);
                float inc = Hz / plugin->sampleRate;
                float dB = -60.0f + plugin->velocity * 54; // -6dB max
                float vol = powf(10.0f, dB / 20.0f);

                for (; frame < event.processAudio.endFrame; frame++) {
                    static const float mypi = 3.141592653589793f;

                    float sample = vol * sinf(2 * mypi * phase);

                    for (int ch = 0; ch < 2; ch++)
                        output[ch][frame] = sample;

                    phase += inc;
                    phase -= (int)phase;
                }

                plugin->oscPhase = phase;
            }
            break;
        }
        default:
            break;
        }
    }
    RESTORE_DENORMALS
}

/* --------------------------------------------------------------------------------------------------------
 * State */

// In these methods we will use a very basic binary preset format: a flat array
// of param values
struct ParamState {
    uint32_t paramId;
    float value;
};

void cplug_saveState(void *userPlugin, const void *stateCtx,
                     cplug_writeProc writeProc) {
    Plugin *plugin = (Plugin *)userPlugin;

    struct ParamState state[NUM_PARAMS];
    for (int i = 0; i < NUM_PARAMS; i++) {
        state[i].paramId = PARAM_IDS[i];
        state[i].value = plugin->paramValuesAudio[i];
    }
    writeProc(stateCtx, state, sizeof(state));
}

void cplug_loadState(void *userPlugin, const void *stateCtx,
                     cplug_readProc readProc) {
    Plugin *plugin = (Plugin *)userPlugin;

    struct ParamState state[NUM_PARAMS * 2];

    // If your plugin has added/removed parameters, requesting for more data
    // then you actually expect may be a good idea
    int64_t bytesRead = readProc(stateCtx, state, sizeof(state));

    for (int i = 0; i < bytesRead / sizeof(state[0]); i++) {
        uint32_t paramIdx = get_param_index(userPlugin, state[i].paramId);
        if (paramIdx < NUM_PARAMS) {
            plugin->paramValuesAudio[paramIdx] = state[i].value;
            plugin->paramValuesMain[paramIdx] = state[i].value;
            sendParamEventFromMain(plugin, CPLUG_EVENT_PARAM_CHANGE_UPDATE,
                                   state[i].paramId, state[i].value);
        }
    }
}

void sendParamEventFromMain(Plugin *plugin, uint32_t type, uint32_t paramId,
                            double value) {
    int mainToAudioHead = cplug_atomic_load_i32(&plugin->mainToAudioHead) &
                          CPLUG_EVENT_QUEUE_MASK;
    CplugEvent *paramEvent = &plugin->mainToAudioQueue[mainToAudioHead];
    paramEvent->parameter.type = type;
    paramEvent->parameter.id = paramId;
    paramEvent->parameter.value = value;

    cplug_atomic_fetch_add_i32(&plugin->mainToAudioHead, 1);
    cplug_atomic_fetch_and_i32(&plugin->mainToAudioHead,
                               CPLUG_EVENT_QUEUE_MASK);
}

//
// GUI
//

static sg_environment get_sg_environment(void *pw) {
    sg_environment env = {0};
    env.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    env.defaults.sample_count = 1;
#ifdef _WIN32
    env.d3d11.device = pw_get_dx11_device(pw);
    env.d3d11.device_context = pw_get_dx11_device_context(pw);
#else
    env.metal.device = pw_get_metal_device(pw);
#endif
    return env;
}

static sg_swapchain get_sg_swapchain(void *pw, int width, int height) {
    sg_swapchain sc = {0};
    sc.width = width;
    sc.height = height;
    sc.sample_count = 1;
    sc.color_format = SG_PIXELFORMAT_BGRA8;
    sc.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
#ifdef _WIN32
    sc.d3d11.render_view = pw_get_dx11_render_target_view(pw);
    sc.d3d11.depth_stencil_view = pw_get_dx11_depth_stencil_view(pw);
#else
    sc.metal.current_drawable = pw_get_metal_drawable(pw);
    sc.metal.depth_stencil_texture = pw_get_metal_depth_stencil_texture(pw);
#endif
    return sc;
}

void imgui_set_scale(GUI *gui, float scale) {
    CPLUG_LOG_ASSERT(gui->imgui_ctx != NULL)
    ImGui_SetCurrentContext(gui->imgui_ctx);

    ImGuiStyle *style = ImGui_GetStyle();
    ImGuiStyle_ScaleAllSizes(style, scale);
    style->FontScaleDpi = scale;
}

static void gui_set_current_context(GUI *gui) {
    ImGui_SetCurrentContext(gui->imgui_ctx);
    sg_set_current_context(gui->sokol_gfx_ctx);
    simgui_set_current_context(gui->sokol_imgui_ctx);
}

void pw_get_info(PWGetInfo *info) {
    if (info->type == PW_INFO_INIT_SIZE) {
        Plugin *plugin = (Plugin *)info->init_size.plugin;
        // info->init_size.width = plugin->width;
        // info->init_size.height = plugin->height;
        info->init_size.width = GUI_DEFAULT_WIDTH;
        info->init_size.height = GUI_DEFAULT_HEIGHT;
    } else if (info->type == PW_INFO_CONSTRAIN_SIZE) {
        // if (info->constrain_size.width > 1024)
        //     info->constrain_size.width = 1024;
        // if (info->constrain_size.height > 500)
        //     info->constrain_size.height = 500;
    }
}

void *pw_create_gui(void *_plugin, void *pw) {
    Plugin *plugin = _plugin;
    GUI *gui = calloc(1, sizeof(*gui));

    plugin->gui = gui;
    gui->plugin = plugin;
    gui->pw = pw;

    gui->normalized_width = GUI_DEFAULT_WIDTH;
    gui->normalized_height = GUI_DEFAULT_HEIGHT;

    gui->clear_color = (ImVec4){0.45f, 0.55f, 0.60f, 1.00f};
    gui->counter = 0;
    gui->f = 0.0f;
    gui->mouse_x = 0;
    gui->mouse_y = 0;
    gui->mouse_button_pressed = 0;

    CIMGUI_CHECKVERSION();

    gui->sokol_gfx_ctx = sg_make_context();
    sg_set_current_context(gui->sokol_gfx_ctx);
    sg_setup(&(sg_desc){
        .environment = get_sg_environment(pw),
        .logger.func = slog_func,
    });

    gui->sokol_imgui_ctx = simgui_make_context();
    simgui_set_current_context(gui->sokol_imgui_ctx);
    // needed for multi-instance initialization
    ImGui_SetCurrentContext(NULL);
    simgui_setup(&(simgui_desc_t){
        .no_default_font = true,
        .color_format = SG_PIXELFORMAT_BGRA8,
        .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
        .sample_count = 1,
        .logger.func = slog_func,
    });

    gui->imgui_ctx = ImGui_GetCurrentContext();

    ImGuiIO *io = ImGui_GetIO();
    io->IniFilename = NULL;

    const char *font_name = "Iosevka-Regular.ttf";
    char *path = NULL;
    int length, dirname_length;
    length = wai_getModulePath(NULL, 0, &dirname_length);
    PW_ASSERT(length > 0);
    path = (char *)malloc(length + 1 + strlen(font_name));
    wai_getModulePath(path, length, &dirname_length);
    path[dirname_length + 1] = '\0';
    strcat(path, font_name);
    cplug_log("font path: %s\n", path);
    FILE *font_file = fopen(path, "rb");
    if (font_file) {
        fclose(font_file);
        gui->font =
            ImFontAtlas_AddFontFromFileTTF(io->Fonts, path, 0.0f, NULL, NULL);
    } else {
        cplug_log("font not found at %s, using default\n", path);
        ImFontAtlas_AddFontDefault(io->Fonts, NULL);
        gui->font = NULL;
    }
    free(path);

    // Setup Dear ImGui style
    ImGui_StyleColorsDark(NULL);

    // TODO: try to get rid of this
    // currently needed for hosts such as Reason that ignore VST3 scaling
    // callbacks
#ifdef _WIN32
    DPI_AWARENESS awareness =
        GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
    if (awareness != DPI_AWARENESS_UNAWARE) {
        HMONITOR monitor = MonitorFromWindow(pw_get_native_window(pw),
                                             MONITOR_DEFAULTTONEAREST);
        DEVICE_SCALE_FACTOR device_scale_factor;
        HRESULT hr = GetScaleFactorForMonitor(monitor, &device_scale_factor);
        PW_ASSERT(hr == S_OK);
        float scale = device_scale_factor / 100.0f;
        cplug_setScaleFactor(gui->pw, scale);
        cplug_setSize(gui->pw, gui->normalized_width * scale,
                      gui->normalized_height * scale);
    }
#endif // _WIN32

    float scale = pw_get_content_scale_factor(gui->pw);
    imgui_set_scale(gui, scale);

    return gui;
}

void pw_destroy_gui(void *_gui) {
    GUI *gui = (GUI *)_gui;

    gui_set_current_context(gui);

    simgui_shutdown();
    sg_shutdown();
    sg_destroy_context(gui->sokol_gfx_ctx);
    simgui_destroy_context(gui->sokol_imgui_ctx);
    gui->sokol_gfx_ctx = NULL;
    gui->sokol_imgui_ctx = NULL;

    gui->plugin->gui = NULL;
    free(gui);
}

void pw_tick(void *_gui) {
    GUI *gui = (GUI *)_gui;

    gui_set_current_context(gui);

    float scale = pw_get_content_scale_factor(gui->pw);
    float width = (float)(gui->normalized_width) * scale;
    float height = (float)(gui->normalized_height) * scale;

    ImGuiIO *io = ImGui_GetIO();

// #ifndef _WIN32
//     // The timer can fire before the MTKView has a drawable; skip the frame rather
//     // than trip pw_get_metal_drawable()'s assert (used in pw_sg_swapchain below).
//     if (!pw_metal_drawable_ready(gui->pw))
//         return;
// #endif

#ifdef _WIN32
    float fbscale = 1.0f;
#else
    float fbscale = pw_get_backing_scale_factor(gui->pw);
#endif
    int fb_width = (int)(width * fbscale);
    int fb_height = (int)(height * fbscale);

    simgui_new_frame(&(simgui_frame_desc_t){
        .width = fb_width,
        .height = fb_height,
        .delta_time = 1.0 / 60.0,
        .dpi_scale = fbscale,
    });
    if (gui->font)
        ImGui_PushFontFloat(gui->font, 18.0f);
    ImGuiStyle *style = ImGui_GetStyle();
    style->WindowPadding = (ImVec2){20.0f, 20.0f};
    style->FrameRounding = 10.0f;
    style->FramePadding = (ImVec2){10.0f, 5.0f};
    style->GrabRounding = 10.0f;
    style->PopupRounding = 10.0f;
    style->ScrollbarRounding = 10.0f;
    style->TabRounding = 10.0f;
    style->ChildRounding = 10.0f;

    ImGui_SetNextWindowPos((ImVec2){0, 0}, 0);
    ImGui_SetNextWindowSize((ImVec2){width, height}, 0);
    ImGui_Begin("Demo Plugin", 0,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

    ImGui_Text("This is some useful text.");
    ImGui_SliderFloat("float", &gui->f, 0.0f, 1.0f);
    ImGui_ColorEdit3("clear color", (float *)&gui->clear_color, 0);

    if (ImGui_Button("Button"))
        gui->counter++;

    ImGui_SameLine();
    ImGui_Text("counter = %d", gui->counter);
    ImGui_Text("f = %f, mouse X = %4d, mouse Y = %4d, button = %d", gui->f,
               gui->mouse_x, gui->mouse_y, gui->mouse_button_pressed);

    ImGui_Text("Application average %.3f ms/frame (%.1f FPS)",
               1000.0f / io->Framerate, io->Framerate);
    ImGui_End();
    if (gui->font)
        ImGui_PopFont();

    sg_pass_action action = {0};
    action.colors[0].load_action = SG_LOADACTION_CLEAR;
    action.colors[0].clear_value = (sg_color){
        gui->clear_color.x * gui->clear_color.w,
        gui->clear_color.y * gui->clear_color.w,
        gui->clear_color.z * gui->clear_color.w,
        gui->clear_color.w,
    };

    sg_begin_pass(&(sg_pass){
        .action = action,
        .swapchain = get_sg_swapchain(gui->pw, fb_width, fb_height),
    });
    simgui_render();
    sg_end_pass();
    sg_commit();
}

bool pw_event(const PWEvent *event) {
    GUI *gui = event->gui;
    Plugin *plugin = gui->plugin;
    ImGuiIO *io;
    float scale;
    switch (event->type) {
    case PW_EVENT_RESIZE_UPDATE:
        scale = pw_get_content_scale_factor(gui->pw);
        gui->normalized_width = event->resize.width / scale;
        gui->normalized_height = event->resize.height / scale;
        break;
    case PW_EVENT_MOUSE_MOVE:
        ImGui_SetCurrentContext(gui->imgui_ctx);
        io = ImGui_GetIO();
        ImGuiIO_AddMousePosEvent(io, event->mouse.x, event->mouse.y);
        gui->mouse_x = (int)event->mouse.x;
        gui->mouse_y = (int)event->mouse.y;
        break;
    case PW_EVENT_MOUSE_LEFT_DOWN:
        ImGui_SetCurrentContext(gui->imgui_ctx);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 0, true);
        gui->mouse_button_pressed = 1;
        break;
    case PW_EVENT_MOUSE_LEFT_UP:
        ImGui_SetCurrentContext(gui->imgui_ctx);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 0, false);
        gui->mouse_button_pressed = 0;
        break;
    case PW_EVENT_MOUSE_RIGHT_DOWN:
        ImGui_SetCurrentContext(gui->imgui_ctx);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 1, true);
        gui->mouse_button_pressed = 1;
        break;
    case PW_EVENT_MOUSE_RIGHT_UP:
        ImGui_SetCurrentContext(gui->imgui_ctx);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 1, false);
        gui->mouse_button_pressed = 0;
        break;
    case PW_EVENT_CONTENT_SCALE_FACTOR_CHANGED:
        scale = event->content_scale_factor;
        uint32_t width = (uint32_t)(scale * gui->normalized_width);
        uint32_t height = (uint32_t)(scale * gui->normalized_height);

        if (plugin->hostContext->type != CPLUG_PLUGIN_IS_STANDALONE)
            plugin->hostContext->requestResize(plugin->hostContext, width,
                                               height);
        else
            cplug_setSize(gui->pw, width, height);

        imgui_set_scale(gui, event->content_scale_factor);

        break;
    default:
        break;
    }
    return false;
}
