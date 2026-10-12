#include "host_dpi.h"

#include <cplug.h>
#include <cplug_extensions/window.h>

#ifdef _WIN32
#include <math.h>
#include <windows.h>

// Loaded at runtime: these need Windows 10 1607, and CPLUG supports Windows 8,
// where importing them directly would stop the plugin from loading.
typedef UINT(WINAPI *GetDpiForWindowFn)(HWND);
typedef DPI_AWARENESS_CONTEXT(WINAPI *GetThreadDpiAwarenessContextFn)(void);
typedef DPI_AWARENESS(WINAPI *GetAwarenessFromDpiAwarenessContextFn)(
    DPI_AWARENESS_CONTEXT);

void host_dpi_tick(HostDpi *d, void *pw) {
    if (d->host_scales)
        return;

    HMODULE user32 = GetModuleHandleW(L"user32");
    GetDpiForWindowFn get_dpi =
        (GetDpiForWindowFn)(void *)GetProcAddress(user32, "GetDpiForWindow");
    GetThreadDpiAwarenessContextFn get_context =
        (GetThreadDpiAwarenessContextFn)(void *)GetProcAddress(
            user32, "GetThreadDpiAwarenessContext");
    GetAwarenessFromDpiAwarenessContextFn get_awareness =
        (GetAwarenessFromDpiAwarenessContextFn)(void *)GetProcAddress(
            user32, "GetAwarenessFromDpiAwarenessContext");
    if (!get_dpi || !get_context || !get_awareness)
        return;
    // In a DPI-unaware host, Windows scales the whole window itself.
    if (get_awareness(get_context()) == DPI_AWARENESS_UNAWARE)
        return;

    UINT dpi = get_dpi((HWND)pw_get_native_window(pw));
    if (dpi == 0 || dpi == d->last_dpi)
        return;
    d->last_dpi = dpi;

    float scale = (float)dpi / USER_DEFAULT_SCREEN_DPI;
    if (fabsf(scale - pw_get_content_scale_factor(pw)) < 0.001f)
        return;
    d->applying = true;
    cplug_setScaleFactor(pw, scale);
    d->applying = false;
}
#else
void host_dpi_tick(HostDpi *d, void *pw) {
    (void)d;
    (void)pw;
}
#endif

void host_dpi_on_scale_event(HostDpi *d) {
    if (!d->applying)
        d->host_scales = true;
}
