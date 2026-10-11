// Scrolling 3D spectrogram (a waterfall) seen from an angle. The front line
// shows the latest analysis. Every ticks_per_row analyses, the peak levels
// over that span become a row, which glides back into the distance.
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include "clay_sokol.h"

// 4096 points give ~11 Hz bins at 44.1-48 kHz, fine enough for the low bands.
#define SPECTRUM_FFT_SIZE      4096
#define SPECTRUM_BANDS         64
#define SPECTRUM_ROWS          48

// Zero-initialized means "silent history".
typedef struct Spectrum {
    float rows[SPECTRUM_ROWS][SPECTRUM_BANDS]; // levels 0-1, 30 Hz up
    int newest;                                // index of the latest row
    float current[SPECTRUM_BANDS];             // the latest analysis
    float held[SPECTRUM_BANDS];                // peaks since the latest row
    int held_count;                            // analyses in held
    int ticks_per_row;                         // from the latest spectrum_push
    float re[SPECTRUM_FFT_SIZE];               // FFT scratch, kept off the stack
    float im[SPECTRUM_FFT_SIZE];
} Spectrum;

typedef struct SpectrumStyle {
    Clay_Color front;   // the latest analysis
    Clay_Color history; // rows, faded with age
    Clay_Color floor;   // the outline under the rows
    float px_scale;     // pixels per layout unit, for line widths
    float glow;         // scales glow radius and strength: 0 off, 1 default
} SpectrumStyle;

// Analyzes SPECTRUM_FFT_SIZE samples (oldest first). ticks_per_row is the
// number of analyses per history row (at least 1). Does nothing until
// sample_rate is known (> 0).
void spectrum_push(Spectrum *s, const float *samples, float sample_rate,
                   int ticks_per_row);

// box is in pixels, as passed to a ClaySokolCustomFn.
void spectrum_draw(ClaySokol *cs, Clay_BoundingBox box, const Spectrum *s,
                   const SpectrumStyle *style);

#endif // SPECTRUM_H
