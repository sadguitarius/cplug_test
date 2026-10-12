// Content scale for Windows hosts that are DPI-aware but never send plugins a
// scale (Reason, for example). host_dpi_tick() applies the editor window's DPI
// through cplug_setScaleFactor() and follows it when the window moves to a
// monitor with another DPI. Once the host sends a scale itself, the host's
// scale wins and host_dpi stops. Does nothing on other platforms.
#ifndef HOST_DPI_H
#define HOST_DPI_H

#include <stdbool.h>

// Zero-initialized before the editor's first tick.
typedef struct HostDpi {
    bool applying;     // inside host_dpi's own cplug_setScaleFactor() call
    bool host_scales;  // the host sent a scale, so host_dpi stays out of it
    unsigned last_dpi; // the window DPI last seen, 0 before the first tick
} HostDpi;

// Call at the start of every pw_tick(). By then CPLUG has stored the GUI
// pointer, so the scale event it sends reaches the editor.
void host_dpi_tick(HostDpi *d, void *pw);

// Call on every PW_EVENT_CONTENT_SCALE_FACTOR_CHANGED.
void host_dpi_on_scale_event(HostDpi *d);

#endif // HOST_DPI_H
