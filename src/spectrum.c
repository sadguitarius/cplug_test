#include "spectrum.h"

#include <math.h>

#define PI       3.14159265f
#define MIN_HZ   30.0f
#define MAX_HZ   20000.0f
#define FLOOR_DB -90.0f // level 0; 0 dBFS is level 1

// The waterfall in world space: x across the bands, y up (0 is the floor),
// z into the distance (0 is the front line).
#define HALF_WIDTH  1.6f
#define PEAK_HEIGHT 0.9f // y of a full-scale level
#define DEPTH       3.0f // z of the oldest row
// The view: turned sideways (yaw) and tilted down (pitch), radians.
#define YAW         0.55f
#define PITCH       0.45f
#define CAMERA_DIST 7.0f // from the waterfall's center; larger is flatter
#define FIT_MARGIN  0.06f // part of the box kept clear on each side

// Glow radius (layout units) and strength (times the line's opacity) at
// SpectrumStyle.glow == 1.
#define FRONT_GLOW         10.0f
#define FRONT_GLOW_ALPHA   0.45f
#define HISTORY_GLOW       5.0f
#define HISTORY_GLOW_ALPHA 0.25f

// In-place iterative radix-2 FFT. n must be a power of two.
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                float next_cr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next_cr;
            }
        }
    }
}

void spectrum_push(Spectrum *s, const float *samples, float sample_rate) {
    enum { N = SPECTRUM_FFT_SIZE };
    if (sample_rate <= 0.0f)
        return;

    float *re = s->re, *im = s->im;
    for (int i = 0; i < N; i++) {
        float hann = 0.5f - 0.5f * cosf(2.0f * PI * (float)i / (float)(N - 1));
        re[i] = samples[i] * hann;
        im[i] = 0.0f;
    }
    fft(re, im, N);
    for (int k = 0; k <= N / 2; k++)
        re[k] = sqrtf(re[k] * re[k] + im[k] * im[k]); // re now holds magnitudes

    float *row = s->current;
    float top_hz = fminf(MAX_HZ, sample_rate * 0.5f);
    float bin_hz = sample_rate / (float)N;
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float f0 = MIN_HZ * powf(top_hz / MIN_HZ, (float)b / SPECTRUM_BANDS);
        float f1 = MIN_HZ * powf(top_hz / MIN_HZ, (float)(b + 1) / SPECTRUM_BANDS);
        float k0 = f0 / bin_hz, k1 = f1 / bin_hz;
        float peak = 0.0f;
        if (k1 - k0 < 1.0f) {
            // Narrower than a bin: interpolate at the band's center.
            float k = sqrtf(k0 * k1);
            int i = (int)k;
            if (i > N / 2 - 1)
                i = N / 2 - 1;
            float t = k - (float)i;
            peak = re[i] + (re[i + 1] - re[i]) * t;
        } else {
            int last = (int)k1 < N / 2 ? (int)k1 : N / 2;
            for (int i = (int)k0; i <= last; i++)
                peak = fmaxf(peak, re[i]);
        }
        // The Hann window halves a sine's peak, so 4/N maps a full-scale sine to 1.
        float db = 20.0f * log10f(peak * 4.0f / (float)N + 1e-9f);
        float level = (db - FLOOR_DB) / -FLOOR_DB;
        row[b] = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
        // Peak-holding between rows keeps short sounds visible in the history.
        s->held[b] = s->held_any ? fmaxf(s->held[b], row[b]) : row[b];
    }
    s->held_any = true;
}

void spectrum_advance(Spectrum *s, float seconds, float rows_per_second) {
    s->row_phase += seconds * rows_per_second;
    while (s->row_phase >= 1.0f) {
        // A row with no analysis since the previous one repeats the latest.
        const float *levels = s->held_any ? s->held : s->current;
        s->newest = (s->newest + 1) % SPECTRUM_ROWS;
        for (int b = 0; b < SPECTRUM_BANDS; b++)
            s->rows[s->newest][b] = levels[b];
        s->held_any = false;
        s->row_phase -= 1.0f;
    }
}

// Rotates world space into camera space, then projects to a unit image plane;
// screen = offset + scale * unit, so the projection keeps its perspective.
typedef struct View {
    float yaw_s, yaw_c, pitch_s, pitch_c;
    float scale, ox, oy;
} View;

static void project_unit(const View *v, float x, float y, float z, float *ux,
                         float *uy) {
    // Center the waterfall on the origin so it turns in place.
    y -= PEAK_HEIGHT * 0.5f;
    z -= DEPTH * 0.5f;
    float x1 = x * v->yaw_c - z * v->yaw_s;
    float z1 = x * v->yaw_s + z * v->yaw_c;
    // Tilting down raises far points on screen and brings high points closer.
    float y2 = y * v->pitch_c + z1 * v->pitch_s;
    float z2 = z1 * v->pitch_c - y * v->pitch_s + CAMERA_DIST;
    *ux = x1 / z2;
    *uy = -y2 / z2;
}

static void project(const View *v, float x, float y, float z, float *sx, float *sy) {
    float ux, uy;
    project_unit(v, x, y, z, &ux, &uy);
    *sx = v->ox + v->scale * ux;
    *sy = v->oy + v->scale * uy;
}

// Scales and centers the projection so the whole waterfall volume fits the box.
static View fit_view(Clay_BoundingBox box) {
    View v = {sinf(YAW), cosf(YAW), sinf(PITCH), cosf(PITCH), 1.0f, 0.0f, 0.0f};
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (int i = 0; i < 8; i++) {
        float ux, uy;
        project_unit(&v, (i & 1) ? HALF_WIDTH : -HALF_WIDTH,
                     (i & 2) ? PEAK_HEIGHT : 0.0f, (i & 4) ? DEPTH : 0.0f, &ux, &uy);
        min_x = fminf(min_x, ux);
        max_x = fmaxf(max_x, ux);
        min_y = fminf(min_y, uy);
        max_y = fmaxf(max_y, uy);
    }
    float usable = 1.0f - 2.0f * FIT_MARGIN;
    v.scale = fminf(box.width * usable / (max_x - min_x),
                    box.height * usable / (max_y - min_y));
    v.ox = box.x + box.width * 0.5f - v.scale * (min_x + max_x) * 0.5f;
    v.oy = box.y + box.height * 0.5f - v.scale * (min_y + max_y) * 0.5f;
    return v;
}

// Draws one row as a line with a glow under it. The glow uses the line's color
// at glow_alpha times its opacity, fading out over glow pixels.
static void draw_row(ClaySokol *cs, const View *v, const float *levels, float z,
                     float width, Clay_Color color, float glow, float glow_alpha) {
    float pts[SPECTRUM_BANDS][2];
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float x = ((float)b / (SPECTRUM_BANDS - 1) * 2.0f - 1.0f) * HALF_WIDTH;
        project(v, x, levels[b] * PEAK_HEIGHT, z, &pts[b][0], &pts[b][1]);
    }
    Clay_Color halo = color;
    halo.a *= glow_alpha;
    for (int b = 1; b < SPECTRUM_BANDS; b++)
        clay_sokol_glow_line(cs, pts[b - 1][0], pts[b - 1][1], pts[b][0], pts[b][1],
                             width, glow, halo);
    for (int b = 1; b < SPECTRUM_BANDS; b++)
        clay_sokol_line(cs, pts[b - 1][0], pts[b - 1][1], pts[b][0], pts[b][1], width,
                        color);
}

void spectrum_draw(ClaySokol *cs, Clay_BoundingBox box, const Spectrum *s,
                   const SpectrumStyle *style) {
    View v = fit_view(box);
    float px = style->px_scale;

    float c[4][2]; // floor corners: front left, front right, back left, back right
    for (int i = 0; i < 4; i++)
        project(&v, (i & 1) ? HALF_WIDTH : -HALF_WIDTH, 0.0f, (i & 2) ? DEPTH : 0.0f,
                &c[i][0], &c[i][1]);
    clay_sokol_line(cs, c[0][0], c[0][1], c[1][0], c[1][1], px, style->floor);
    clay_sokol_line(cs, c[2][0], c[2][1], c[3][0], c[3][1], px, style->floor);
    clay_sokol_line(cs, c[0][0], c[0][1], c[2][0], c[2][1], px, style->floor);
    clay_sokol_line(cs, c[1][0], c[1][1], c[3][0], c[3][1], px, style->floor);

    // A row starts at the front line when it is made, then glides back one row
    // spacing per row interval. Oldest first, so newer rows draw on top.
    float glide = s->row_phase;
    float g = style->glow;
    for (int age = SPECTRUM_ROWS - 1; age >= 0; age--) {
        const float *row = s->rows[(s->newest - age + SPECTRUM_ROWS) % SPECTRUM_ROWS];
        float place = ((float)age + glide) / SPECTRUM_ROWS; // 0 front, ~1 back
        Clay_Color color = style->history;
        color.a *= 1.0f - place;
        draw_row(cs, &v, row, place * DEPTH, px, color, HISTORY_GLOW * g * px,
                 fminf(HISTORY_GLOW_ALPHA * g, 1.0f));
    }
    draw_row(cs, &v, s->current, 0.0f, 2.0f * px, style->front, FRONT_GLOW * g * px,
             fminf(FRONT_GLOW_ALPHA * g, 1.0f));
}
