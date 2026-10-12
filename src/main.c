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

#include <clay.h>

#include "clay_sokol.h"
#include "gui_libs.h"
#include "host_dpi.h"
#include "spectrum.h"

#ifdef _WIN32
#define my_assert(cond) (cond) ? (void)0 : __debugbreak()
#else
#define my_assert(cond) (cond) ? (void)0 : __builtin_debugtrap()
#endif // _WIN32

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
// https://softwareengineering.stackexchange.com/a/337251
#include <pmmintrin.h>
#define DISABLE_DENORMALS                                                      \
    unsigned int oldMXCSR = _mm_getcsr(); /*read the old MXCSR setting  */     \
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);                                \
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#define RESTORE_DENORMALS _mm_setcsr(oldMXCSR);
#else
#include <fenv.h>
#if defined(__x86_64__)
#define DISABLE_DENORMS_ENV FE_DFL_DISABLE_SSE_DENORMS_ENV
#elif defined(__arm64__)
#define DISABLE_DENORMS_ENV FE_DFL_DISABLE_DENORMS_ENV
#endif // x86, ARM64

#define DISABLE_DENORMALS                                                      \
    fenv_t _fenv;                                                              \
    fegetenv(&_fenv);                                                          \
    fesetenv(DISABLE_DENORMS_ENV);
#define RESTORE_DENORMALS fesetenv(&_fenv);
#endif

#define ARRLEN(a)              (sizeof(a) / sizeof((a)[0]))
#define CPLUG_EVENT_QUEUE_MASK (CPLUG_EVENT_QUEUE_SIZE - 1)

static const uint32_t PARAM_IDS[] = {
    'pf32',
    'pi32',
    'bool',
    'utf8',
    'shap',
};
enum { NUM_PARAMS = ARRLEN(PARAM_IDS) };

static const char *const SHAPE_NAMES[] = {"Sine", "Square", "Saw"};
enum { NUM_SHAPES = ARRLEN(SHAPE_NAMES) };

#define GUI_DEFAULT_WIDTH  1040
#define GUI_DEFAULT_HEIGHT 440
#define GUI_TICK_SECONDS   (1.0f / 60.0f)
#define IMGUI_INITIAL_WIDTH 560 // layout units, until ImGui reports its fitted width

// Output samples kept for the spectrogram; a power of two.
#define SCOPE_SIZE 8192
#define SCOPE_MASK (SCOPE_SIZE - 1)

#define FONT_NAME "Lilex-Regular.ttf"

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

    // Editor settings that outlive the editor window.
    ImVec4 clearColor;
    float scopeGlow;  // spectrogram glow: 0 off, 1 default, up to 2
    float scopeSpeed; // spectrogram history rows per second

    float paramValuesMain[NUM_PARAMS];

    // Single reader writer queue. Pretty sure atomics aren't required, but here
    // anyway
    cplug_atomic_i32 mainToAudioHead;
    cplug_atomic_i32 mainToAudioTail;
    CplugEvent mainToAudioQueue[CPLUG_EVENT_QUEUE_SIZE];

    cplug_atomic_i32 audioToMainHead;
    cplug_atomic_i32 audioToMainTail;
    CplugEvent audioToMainQueue[CPLUG_EVENT_QUEUE_SIZE];

    // Ring of output samples for the GUI's spectrogram. The audio thread
    // writes and advances scopeHead; the GUI reads the newest samples
    // without locking. A read that races a write only glitches the display.
    cplug_atomic_i32 scopeHead;
    float scope[SCOPE_SIZE];
} Plugin;

void sendParamEventFromMain(Plugin *plugin, uint32_t type, uint32_t paramIdx,
                            double value);

typedef struct GUI {
    Plugin *plugin;
    void *pw;
#ifdef _WIN32
    char uniqueClassName[64];
#endif

    float normalized_width;
    float normalized_height;

    bool mouseDragging;
    uint32_t dragParamId;
    int dragStartX;
    int dragStartY;
    double dragStartParamNormalised;
    double dragCurrentParamNormalised;

    GuiLibs *libs; // sokol_gfx, sokol_imgui, ImGui and Clay state for this editor
    ClaySokol *clay_sokol;
    float px_scale; // pixels per Clay layout unit

    // Pointer in Clay layout units. pointer_pressed latches a left click
    // between ticks.
    float pointer_x;
    float pointer_y;
    bool pointer_down;
    bool pointer_pressed;

    Spectrum spectrum;
    int last_scope_head;

    float imgui_width; // the ImGui window's fitted width, in layout units

    HostDpi host_dpi; // scale for DPI-aware hosts that never send one

    ImFont *font;

    ImGuiStyle imgui_style;

    // TODO: move to state struct!!
    // Our state
    int counter;
    float f;
    int mouse_x;
    int mouse_y;
    int mouse_button_pressed;
} GUI;

static int shape_index(double value) {
    int shape = (int)lround(value);
    return shape < 0 ? 0 : shape >= NUM_SHAPES ? NUM_SHAPES - 1 : shape;
}

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

    // 'shap'
    idx = get_param_index(plugin, 'shap');
    plugin->paramValuesAudio[idx] = 0.0f;
    plugin->paramInfo[idx].flags =
        CPLUG_FLAG_PARAMETER_IS_AUTOMATABLE | CPLUG_FLAG_PARAMETER_IS_INTEGER;
    plugin->paramInfo[idx].min = 0.0f;
    plugin->paramInfo[idx].max = (float)(NUM_SHAPES - 1);
    plugin->paramInfo[idx].defaultValue = 0.0f;

    plugin->midiNote = -1;
    plugin->clearColor = (ImVec4){0.45f, 0.55f, 0.60f, 1.00f};
    plugin->scopeGlow = 1.0f;
    plugin->scopeSpeed = 12.0f;

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
                                        "UTF8 Приве́т नमस्ते שָׁלוֹם 🐨",
                                        "Waveform"};
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
    else if (paramId == 'shap')
        snprintf(buf, bufsize, "%s", SHAPE_NAMES[shape_index(value)]);
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

// Audio thread only: the single writer of plugin->scope.
static void push_scope(Plugin *plugin, const float *samples, uint32_t count) {
    int head = cplug_atomic_load_i32(&plugin->scopeHead) & SCOPE_MASK;
    for (uint32_t i = 0; i < count; i++)
        plugin->scope[(head + (int)i) & SCOPE_MASK] = samples[i];
    cplug_atomic_exchange_i32(&plugin->scopeHead, (head + (int)count) & SCOPE_MASK);
}

// Renders count mono samples of the synth into block. Audio thread only.
static void render_block(Plugin *plugin, float *block, uint32_t count) {
    if (plugin->midiNote == -1) {
        memset(block, 0, sizeof(float) * count);
        return;
    }
    static const float mypi = 3.141592653589793f;
    float phase = plugin->oscPhase;
    float Hz = 440.0f * exp2f(((float)plugin->midiNote - 69.0f) * 0.0833333f);
    float inc = Hz / plugin->sampleRate;
    float dB = -60.0f + plugin->velocity * 54; // -6dB max
    float vol = powf(10.0f, dB / 20.0f);
    int shape =
        shape_index(plugin->paramValuesAudio[get_param_index(plugin, 'shap')]);

    for (uint32_t i = 0; i < count; i++) {
        float sample;
        if (shape == 0)
            sample = sinf(2 * mypi * phase);
        else if (shape == 1)
            sample = phase < 0.5f ? 0.5f : -0.5f;
        else
            sample = (2.0f * phase - 1.0f) * 0.6f;
        block[i] = sample * vol;

        phase += inc;
        phase -= (int)phase;
    }
    plugin->oscPhase = phase;
}

// Copies a mono block to both output channels starting at frame, as float or
// double: the host chooses the sample type (CPLUG_AUDIO_SAMPLE_*).
static void write_output(const CplugProcessContext *ctx, void *output,
                         uint32_t frame, const float *block, uint32_t count) {
    if (ctx->audioSampleType == CPLUG_AUDIO_SAMPLE_FLOAT64) {
        double **out = (double **)output;
        for (int ch = 0; ch < 2; ch++)
            for (uint32_t i = 0; i < count; i++)
                out[ch][frame + i] = block[i];
    } else {
        float **out = (float **)output;
        for (int ch = 0; ch < 2; ch++)
            memcpy(&out[ch][frame], block, sizeof(float) * count);
    }
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

            // Channel pointer arrays, float or double per ctx->audioSampleType.
            void *output = ctx->getAudioOutput(ctx, 0);
            CPLUG_LOG_ASSERT(output != NULL)

            // Render in float, then convert on output; the scope takes float.
            while (frame < event.processAudio.endFrame) {
                float block[256];
                uint32_t count = event.processAudio.endFrame - frame;
                if (count > ARRLEN(block))
                    count = ARRLEN(block);
                render_block(plugin, block, count);
                write_output(ctx, output, frame, block, count);
                push_scope(plugin, block, count);
                frame += count;
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
// of {id, value} pairs. Param values come first, then editor settings under
// ids that are not params. Loading skips ids it does not know, so states from
// older and newer builds still load.
struct ParamState {
    uint32_t paramId;
    float value;
};

static const uint32_t CLEAR_COLOR_IDS[4] = {'clrR', 'clrG', 'clrB', 'clrA'};
enum { NUM_STATE_ENTRIES = NUM_PARAMS + ARRLEN(CLEAR_COLOR_IDS) };

void cplug_saveState(void *userPlugin, const void *stateCtx,
                     cplug_writeProc writeProc) {
    Plugin *plugin = (Plugin *)userPlugin;

    struct ParamState state[NUM_STATE_ENTRIES];
    for (int i = 0; i < NUM_PARAMS; i++) {
        state[i].paramId = PARAM_IDS[i];
        state[i].value = plugin->paramValuesAudio[i];
    }
    const ImVec4 c = plugin->clearColor;
    const float rgba[4] = {c.x, c.y, c.z, c.w};
    for (int i = 0; i < 4; i++) {
        state[NUM_PARAMS + i].paramId = CLEAR_COLOR_IDS[i];
        state[NUM_PARAMS + i].value = rgba[i];
    }
    writeProc(stateCtx, state, sizeof(state));
}

// Sets one clear color component from its state id; ignores other ids.
static void load_clear_color_component(Plugin *plugin, uint32_t id, float value) {
    if (id == CLEAR_COLOR_IDS[0])
        plugin->clearColor.x = value;
    else if (id == CLEAR_COLOR_IDS[1])
        plugin->clearColor.y = value;
    else if (id == CLEAR_COLOR_IDS[2])
        plugin->clearColor.z = value;
    else if (id == CLEAR_COLOR_IDS[3])
        plugin->clearColor.w = value;
}

void cplug_loadState(void *userPlugin, const void *stateCtx,
                     cplug_readProc readProc) {
    Plugin *plugin = (Plugin *)userPlugin;

    struct ParamState state[NUM_STATE_ENTRIES * 2];

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
        } else {
            load_clear_color_component(plugin, state[i].paramId, state[i].value);
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
    gui_libs_enter(gui->libs);

    ImGuiStyle *style = ImGui_GetStyle();
    *style = gui->imgui_style;
    ImGuiStyle_ScaleAllSizes(style, scale);
    style->FontScaleDpi = scale;
}

// must call on every host callback entry point to preserve multi-instance
// behavior
static void gui_set_current_context(GUI *gui) { gui_libs_enter(gui->libs); }

// Returns the malloc'd path of a file next to the plugin binary.
static char *module_file_path(const char *name) {
    int dirname_length;
    int length = wai_getModulePath(NULL, 0, &dirname_length);
    PW_ASSERT(length > 0);
    char *path = (char *)malloc(length + 1 + strlen(name));
    wai_getModulePath(path, length, &dirname_length);
    path[dirname_length + 1] = '\0';
    strcat(path, name);
    return path;
}

// Returns the malloc'd contents of a file, or NULL.
static unsigned char *read_file(const char *path) {
    unsigned char *data = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = size > 0 ? malloc((size_t)size) : NULL;
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
        fclose(f);
    }
    return data;
}

// Applies parameter changes from the host to the GUI's copy.
static void gui_drain_audio_events(GUI *gui) {
    Plugin *plugin = gui->plugin;
    int head = cplug_atomic_load_i32(&plugin->audioToMainHead) &
               CPLUG_EVENT_QUEUE_MASK;
    int tail = cplug_atomic_load_i32(&plugin->audioToMainTail);
    while (tail != head) {
        const CplugEvent *event = &plugin->audioToMainQueue[tail];
        uint32_t idx = get_param_index(plugin, event->parameter.id);
        if (event->type == CPLUG_EVENT_PARAM_CHANGE_UPDATE && idx < NUM_PARAMS)
            plugin->paramValuesMain[idx] = (float)event->parameter.value;
        tail = (tail + 1) & CPLUG_EVENT_QUEUE_MASK;
    }
    cplug_atomic_exchange_i32(&plugin->audioToMainTail, tail);
}

// Analyzes the newest output samples, if any arrived since the last tick.
static void gui_update_spectrum(GUI *gui) {
    Plugin *plugin = gui->plugin;
    int head = cplug_atomic_load_i32(&plugin->scopeHead) & SCOPE_MASK;
    if (head == gui->last_scope_head)
        return;
    gui->last_scope_head = head;
    float samples[SPECTRUM_FFT_SIZE];
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
        samples[i] = plugin->scope[(head - SPECTRUM_FFT_SIZE + i) & SCOPE_MASK];
    // One analysis per tick, so rows per second sets the ticks per row.
    int ticks_per_row = (int)lroundf(1.0f / (plugin->scopeSpeed * GUI_TICK_SECONDS));
    spectrum_push(&gui->spectrum, samples, plugin->sampleRate, ticks_per_row);
}

// A one-shot parameter change, as a host expects from a button.
static void gui_param_click(GUI *gui, uint32_t paramId, double value) {
    gui->plugin->paramValuesMain[get_param_index(gui->plugin, paramId)] = (float)value;
    sendParamEventFromMain(gui->plugin, CPLUG_EVENT_PARAM_CHANGE_BEGIN, paramId, value);
    sendParamEventFromMain(gui->plugin, CPLUG_EVENT_PARAM_CHANGE_UPDATE, paramId, value);
    sendParamEventFromMain(gui->plugin, CPLUG_EVENT_PARAM_CHANGE_END, paramId, value);
}

/* --------------------------------------------------------------------------------------------------------
 * Clay layout: Clay places everything, and ImGui draws its window into the
 * ImGuiArea element. Clay's colors and rounding come from the ImGui style, so
 * both halves share one look. */

static Clay_Color im_color(const GUI *gui, ImGuiCol idx) {
    ImVec4 c = gui->imgui_style.Colors[idx];
    return (Clay_Color){c.x * 255.0f, c.y * 255.0f, c.z * 255.0f, c.w * 255.0f};
}

static Clay_String cstr(const char *s) {
    return (Clay_String){.isStaticallyAllocated = true,
                         .length = (int32_t)strlen(s),
                         .chars = s};
}

static void ui_waveform_buttons(GUI *gui) {
    uint32_t shap = get_param_index(gui->plugin, 'shap');
    int current = shape_index(gui->plugin->paramValuesMain[shap]);
    Clay_Color text = im_color(gui, ImGuiCol_Text);

    CLAY(CLAY_ID("WaveformRow"),
         {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                     .childGap = 8,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY(CLAY_ID("WaveformLabel"),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT(0)}}}) {
            CLAY_TEXT(CLAY_STRING("Waveform"), {.fontSize = 16, .textColor = text});
        }
        for (int i = 0; i < NUM_SHAPES; i++) {
            Clay_ElementId id = CLAY_IDI("WaveformButton", i);
            if (i != current && gui->pointer_pressed && Clay_PointerOver(id)) {
                gui_param_click(gui, 'shap', i);
                current = i;
            }
            ImGuiCol bg = i == current           ? ImGuiCol_ButtonActive
                          : Clay_PointerOver(id) ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_Button;
            CLAY(id, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                 .childAlignment = {CLAY_ALIGN_X_CENTER,
                                                    CLAY_ALIGN_Y_CENTER}},
                      .backgroundColor = im_color(gui, bg),
                      .cornerRadius = CLAY_CORNER_RADIUS(gui->imgui_style.FrameRounding)}) {
                CLAY_TEXT(cstr(SHAPE_NAMES[i]), {.fontSize = 16, .textColor = text});
            }
        }
    }
}

static Clay_RenderCommandArray ui_layout(GUI *gui) {
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"),
         {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = CLAY_PADDING_ALL(8),
                     .childGap = 8}}) {
        CLAY(CLAY_ID("ImGuiArea"),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(gui->imgui_width),
                                    CLAY_SIZING_GROW(0)}}}) {}

        // Styled like an ImGui window.
        CLAY(CLAY_ID("Panel"),
             {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .padding = CLAY_PADDING_ALL(12),
                         .childGap = 10,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = im_color(gui, ImGuiCol_WindowBg),
              .cornerRadius = CLAY_CORNER_RADIUS(gui->imgui_style.WindowRounding),
              .border = {.color = im_color(gui, ImGuiCol_Border),
                         .width = {1, 1, 1, 1, 0}}}) {
            ui_waveform_buttons(gui);
            CLAY_TEXT(CLAY_STRING("Output spectrum, 30 Hz - 20 kHz"),
                      {.fontSize = 14, .textColor = im_color(gui, ImGuiCol_TextDisabled)});
            // Styled like an ImGui plot frame. The clip becomes a scissor rect
            // around the spectrogram's lines.
            CLAY(CLAY_ID("SpectrumFrame"),
                 {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                  .backgroundColor = im_color(gui, ImGuiCol_FrameBg),
                  .cornerRadius = CLAY_CORNER_RADIUS(gui->imgui_style.FrameRounding),
                  .clip = {.horizontal = true, .vertical = true}}) {
                CLAY(CLAY_ID("Spectrum"),
                     {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                      .custom = {.customData = &gui->spectrum}}) {}
            }
        }
    }

    return Clay_EndLayout(GUI_TICK_SECONDS);
}

// The only custom element is the spectrogram.
static void ui_draw_custom(ClaySokol *cs, Clay_BoundingBox box, void *customData,
                           void *user) {
    GUI *gui = user;
    spectrum_draw(cs, box, customData,
                  &(SpectrumStyle){
                      .front = im_color(gui, ImGuiCol_CheckMark),
                      .history = im_color(gui, ImGuiCol_PlotLines),
                      .floor = im_color(gui, ImGuiCol_Border),
                      .px_scale = gui->px_scale,
                      .glow = gui->plugin->scopeGlow,
                  });
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

    // allocate up front, so a failure unwinds with nothing set up
    GUI *gui = calloc(1, sizeof(*gui));
    GuiLibs *libs = gui_libs_create();
    char *font_path = module_file_path(FONT_NAME);
    unsigned char *ttf = read_file(font_path);
    if (gui == NULL || libs == NULL || ttf == NULL) {
        if (ttf == NULL)
            cplug_log("font not found at %s\n", font_path);
        free(gui);
        if (libs)
            gui_libs_destroy(libs);
        free(font_path);
        free(ttf);
        return NULL;
    }

    plugin->gui = gui;
    gui->libs = libs;
    gui->plugin = plugin;
    gui->pw = pw;

    gui->normalized_width = GUI_DEFAULT_WIDTH;
    gui->normalized_height = GUI_DEFAULT_HEIGHT;

    gui->counter = 0;
    gui->f = 0.0f;
    gui->mouse_x = 0;
    gui->mouse_y = 0;
    gui->mouse_button_pressed = 0;
    for (int i = 0; i < NUM_PARAMS; i++)
        plugin->paramValuesMain[i] = plugin->paramValuesAudio[i];
    gui->last_scope_head = cplug_atomic_load_i32(&plugin->scopeHead) & SCOPE_MASK;
    gui->imgui_width = IMGUI_INITIAL_WIDTH;

    CIMGUI_CHECKVERSION();

    // Entering a new instance leaves no ImGui context current, so
    // simgui_setup() creates this instance's own (see gui_libs.h).
    gui_libs_enter(libs);
    sg_setup(&(sg_desc){
        .environment = get_sg_environment(pw),
        .logger.func = slog_func,
    });
    simgui_setup(&(simgui_desc_t){
        .no_default_font = true,
        .color_format = SG_PIXELFORMAT_BGRA8,
        .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
        .sample_count = 1,
        .logger.func = slog_func,
    });

    gui->clay_sokol = clay_sokol_create(ttf); // takes ownership of ttf
    Clay_SetMeasureTextFunction(clay_sokol_measure_text, gui->clay_sokol);

    ImGuiIO *io = ImGui_GetIO();
    io->IniFilename = NULL;

    // ImGui reads its own copy of the font, which it keeps in its atlas.
    gui->font =
        ImFontAtlas_AddFontFromFileTTF(io->Fonts, font_path, 0.0f, NULL, NULL);
    free(font_path);

    // Setup Dear ImGui style
    ImGui_StyleColorsDark(NULL);

    {
        ImGuiStyle *style = ImGui_GetStyle();
        style->WindowPadding = (ImVec2){20.0f, 20.0f};
        style->FramePadding = (ImVec2){10.0f, 5.0f};
        style->FrameRounding = 10.0f;
        style->GrabRounding = 10.0f;
        style->PopupRounding = 10.0f;
        style->ScrollbarRounding = 10.0f;
        style->TabRounding = 10.0f;
        style->ChildRounding = 10.0f;
        gui->imgui_style = *style;
    }

    float scale = pw_get_content_scale_factor(gui->pw);
    imgui_set_scale(gui, scale);

    return gui;
}

void pw_destroy_gui(void *_gui) {
    GUI *gui = (GUI *)_gui;

    gui_set_current_context(gui);

    simgui_shutdown();
    clay_sokol_destroy(gui->clay_sokol);
    sg_shutdown();
    gui_libs_destroy(gui->libs);

    gui->plugin->gui = NULL;
    free(gui);
}

void pw_tick(void *_gui) {
    GUI *gui = (GUI *)_gui;

    gui_set_current_context(gui);
    host_dpi_tick(&gui->host_dpi, gui->pw);
    gui_drain_audio_events(gui);
    gui_update_spectrum(gui);

    float scale = pw_get_content_scale_factor(gui->pw);
    float width = (float)(gui->normalized_width) * scale;
    float height = (float)(gui->normalized_height) * scale;

    ImGuiIO *io = ImGui_GetIO();

#ifdef _WIN32
    float fbscale = 1.0f;
#else
    float fbscale = pw_get_backing_scale_factor(gui->pw);
#endif
    int fb_width = (int)(width * fbscale);
    int fb_height = (int)(height * fbscale);

    gui->px_scale = scale * fbscale;
    if (clay_sokol_set_scale(gui->clay_sokol, gui->px_scale))
        Clay_ResetMeasureTextCache();
    Clay_SetLayoutDimensions(
        (Clay_Dimensions){gui->normalized_width, gui->normalized_height});
    Clay_SetPointerState((Clay_Vector2){gui->pointer_x, gui->pointer_y},
                         gui->pointer_down);
    Clay_RenderCommandArray cmds = ui_layout(gui);
    gui->pointer_pressed = false;
    Clay_BoundingBox area = Clay_GetElementData(CLAY_ID("ImGuiArea")).boundingBox;

    simgui_new_frame(&(simgui_frame_desc_t){
        .width = fb_width,
        .height = fb_height,
        .delta_time = GUI_TICK_SECONDS,
        .dpi_scale = fbscale,
    });
    if (gui->font)
        ImGui_PushFontFloat(gui->font, 18.0f);

    // ImGui coordinates are layout units times the content scale. A width of
    // 0 makes ImGui fit the width to the content; Clay sizes the ImGuiArea
    // column from it on the next tick.
    ImGui_SetNextWindowPos((ImVec2){area.x * scale, area.y * scale}, 0);
    ImGui_SetNextWindowSize((ImVec2){0.0f, area.height * scale}, 0);
    ImGui_Begin("Demo Plugin", 0,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove);
    gui->imgui_width = ImGui_GetWindowWidth() / scale;

    ImGui_Text("This is some useful text.");
    ImGui_SliderFloat("float", &gui->f, 0.0f, 1.0f);
    ImGui_ColorEdit3("clear color", (float *)&gui->plugin->clearColor, 0);
    ImGui_SliderFloatEx("glow", &gui->plugin->scopeGlow, 0.0f, 2.0f, "%.2f", 0);
    ImGui_SliderFloatEx("scope speed", &gui->plugin->scopeSpeed, 1.0f, 60.0f,
                        "%.0f rows/s", ImGuiSliderFlags_Logarithmic);

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
        gui->plugin->clearColor.x * gui->plugin->clearColor.w,
        gui->plugin->clearColor.y * gui->plugin->clearColor.w,
        gui->plugin->clearColor.z * gui->plugin->clearColor.w,
        gui->plugin->clearColor.w,
    };

    simgui_flush();
    clay_sokol_prepare(gui->clay_sokol, cmds, fb_width, fb_height, ui_draw_custom,
                       gui);
    sg_begin_pass(&(sg_pass){
        .action = action,
        .swapchain = get_sg_swapchain(gui->pw, fb_width, fb_height),
    });
    clay_sokol_draw(gui->clay_sokol);
    simgui_draw();
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
        gui_set_current_context(gui);
        io = ImGui_GetIO();
        ImGuiIO_AddMousePosEvent(io, event->mouse.x, event->mouse.y);
        gui->mouse_x = (int)event->mouse.x;
        gui->mouse_y = (int)event->mouse.y;
        scale = pw_get_content_scale_factor(gui->pw);
        gui->pointer_x = event->mouse.x / scale;
        gui->pointer_y = event->mouse.y / scale;
        break;
    case PW_EVENT_MOUSE_LEFT_DOWN:
        gui_set_current_context(gui);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 0, true);
        gui->mouse_button_pressed = 1;
        gui->pointer_down = true;
        gui->pointer_pressed = true;
        break;
    case PW_EVENT_MOUSE_LEFT_UP:
        gui_set_current_context(gui);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 0, false);
        gui->mouse_button_pressed = 0;
        gui->pointer_down = false;
        break;
    case PW_EVENT_MOUSE_RIGHT_DOWN:
        gui_set_current_context(gui);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 1, true);
        gui->mouse_button_pressed = 1;
        break;
    case PW_EVENT_MOUSE_RIGHT_UP:
        gui_set_current_context(gui);
        io = ImGui_GetIO();
        ImGuiIO_AddMouseButtonEvent(io, 1, false);
        gui->mouse_button_pressed = 0;
        break;
    case PW_EVENT_CONTENT_SCALE_FACTOR_CHANGED:
        host_dpi_on_scale_event(&gui->host_dpi);
        scale = event->content_scale_factor;
        uint32_t width = (uint32_t)lroundf(scale * gui->normalized_width);
        uint32_t height = (uint32_t)lroundf(scale * gui->normalized_height);

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
