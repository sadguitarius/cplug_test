#include "defs.h"
#include <cplug.h>
#include <cplug_extensions/window.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define my_assert(cond) (cond) ? (void)0 : __debugbreak()
#else
#define my_assert(cond) (cond) ? (void)0 : __builtin_debugtrap()
#endif

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

#define CPLUG_EVENT_QUEUE_MASK (CPLUG_EVENT_QUEUE_SIZE - 1)

#define GUI_DEFAULT_WIDTH  1024
#define GUI_DEFAULT_HEIGHT 500
// #define GUI_RATIO_X 16
// #define GUI_RATIO_Y 9

// returns 'CPLUG_NUM_PARAMS' on failure
uint32_t get_param_index(void *ptr, uint32_t paramId) {
    uint32_t i = 0;
    for (; i < ARRLEN(PARAM_IDS); i++)
        if (paramId == PARAM_IDS[i])
            break;
    return i;
}

void sendParamEventFromMain(Plugin *plugin, uint32_t type, uint32_t paramIdx,
                            double value);

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

    plugin->width = GUI_DEFAULT_WIDTH;
    plugin->height = GUI_DEFAULT_HEIGHT;

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
    static_assert(ARRLEN(param_names) == ARRLEN(PARAM_IDS), "Invalid length");

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

void pw_get_info(PWGetInfo *info) {
    if (info->type == PW_INFO_INIT_SIZE) {
        Plugin *plugin = (Plugin *)info->init_size.plugin;
        info->init_size.width = plugin->width;
        info->init_size.height = plugin->height;
    } else if (info->type == PW_INFO_CONSTRAIN_SIZE) {
        if (info->constrain_size.width > 1024)
            info->constrain_size.width = 1024;
        if (info->constrain_size.height > 500)
            info->constrain_size.height = 500;
    }
}

void *pw_create_gui(void *_plugin, void *pw) {
    Plugin *plugin = _plugin;
    GUI *gui = calloc(1, sizeof(*gui));
    // gui->scale = pw_get_dpi(pw);
    gui->scale = 1.0f;

    plugin->gui = gui;
    gui->plugin = plugin;
    gui->pw = pw;

    const struct PWEvent ev = {
        .type = PW_EVENT_RESIZE_UPDATE,
        .gui = gui,
        .resize = {.width = GUI_DEFAULT_WIDTH, .height = GUI_DEFAULT_HEIGHT}};
    imgui_init(gui);
    imgui_start(gui);
    pw_event(&ev);

    return gui;
}

void pw_destroy_gui(void *_gui) {
    GUI *gui = (GUI *)_gui;

    imgui_deinit(gui);
    gui->plugin->gui = NULL;
    free(gui);
}

void pw_tick(void *_gui) {
    GUI *gui = (GUI *)_gui;
    imgui_tick(gui);
}

bool pw_event(const PWEvent *event) {
    GUI *gui = (GUI *)event->gui;
    switch (event->type) {
    case PW_EVENT_RESIZE_UPDATE:
        gui->plugin->width = event->resize.width;
        gui->plugin->height = event->resize.height;
        break;
    case PW_EVENT_MOUSE_MOVE:
    case PW_EVENT_MOUSE_LEFT_DOWN:
    case PW_EVENT_MOUSE_LEFT_UP:
    case PW_EVENT_MOUSE_RIGHT_DOWN:
    case PW_EVENT_MOUSE_RIGHT_UP:
        imgui_handle_event(gui, event);
        break;
    case PW_EVENT_DPI_CHANGED:
        gui->scale = event->dpi;
        imgui_deinit(gui);
        imgui_init(gui);
        imgui_start(gui);
    default:
        break;
    }
    return false;
}
