#include "clay_sokol.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sokol_gfx.h>

#define CLAY_IMPLEMENTATION
#include <clay.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#define MAX_QUADS      8192
#define MAX_VERTS      (6 * MAX_QUADS)
#define MAX_BATCHES    256
#define MAX_FONT_SIZES 8
#define FIRST_CHAR     32 // printable ASCII only
#define NUM_CHARS      95

// rect/shape drive the rounded-rect SDF in the fragment shader:
// rect = (offset from quad center, half size), shape = (corner radius, edge).
// edge 0 fills the shape, edge > 0 draws a ring of that width, and edge < 0
// draws a glow that fades out over -edge pixels outside the shape.
// uv.x < 0 means "untextured".
typedef struct Vertex {
    float x, y, u, v;
    uint32_t color;
    float rect[4];
    float shape[2];
} Vertex;

typedef struct Batch {
    int first, count;
    sg_view view;
    int sx, sy, sw, sh;
} Batch;

// One baked pixel size of the font.
typedef struct Glyphs {
    int px, w, h;
    float ascent;
    stbtt_bakedchar chars[NUM_CHARS];
    sg_image img;
    sg_view view;
} Glyphs;

struct ClaySokol {
    unsigned char *ttf;
    stbtt_fontinfo font;
    float scale;

    sg_shader shd;
    sg_pipeline pip;
    sg_buffer vbuf;
    sg_sampler smp;
    sg_image white_img;
    sg_view white_view;

    Glyphs glyphs[MAX_FONT_SIZES];
    int num_glyphs;

    int fb_width, fb_height;
    int sx, sy, sw, sh; // current scissor rect in pixels

    Vertex verts[MAX_VERTS];
    int num_verts;
    Batch batches[MAX_BATCHES];
    int num_batches;
};

typedef struct VsParams {
    float disp_size[2];
    float pad[2];
} VsParams;

static const char *HLSL_VS =
    "cbuffer params : register(b0) { float2 disp_size; float2 pad; };\n"
    "struct vs_in {\n"
    "    float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; float4 color : TEXCOORD2;\n"
    "    float4 rect : TEXCOORD3; float2 shape : TEXCOORD4;\n"
    "};\n"
    "struct vs_out {\n"
    "    float2 uv : TEXCOORD0; float4 color : TEXCOORD1; float4 rect : TEXCOORD2;\n"
    "    float2 shape : TEXCOORD3; float4 pos : SV_Position;\n"
    "};\n"
    "vs_out main(vs_in i) {\n"
    "    vs_out o;\n"
    "    o.pos = float4(i.pos / disp_size * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);\n"
    "    o.uv = i.uv; o.color = i.color; o.rect = i.rect; o.shape = i.shape;\n"
    "    return o;\n"
    "}\n";

static const char *HLSL_FS =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct vs_out {\n"
    "    float2 uv : TEXCOORD0; float4 color : TEXCOORD1; float4 rect : TEXCOORD2;\n"
    "    float2 shape : TEXCOORD3; float4 pos : SV_Position;\n"
    "};\n"
    "float sd_round_box(float2 p, float2 b, float r) {\n"
    "    float2 q = abs(p) - b + r;\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "float4 main(vs_out i) : SV_Target0 {\n"
    "    float t = tex.Sample(smp, i.uv).r;\n"
    "    if (i.uv.x < 0.0) t = 1.0;\n"
    "    float d = sd_round_box(i.rect.xy, i.rect.zw, i.shape.x);\n"
    "    float cov = saturate(0.5 - d);\n"
    "    if (i.shape.y > 0.0)\n"
    "        cov *= saturate(0.5 + sd_round_box(i.rect.xy, i.rect.zw - i.shape.y,\n"
    "                                           max(i.shape.x - i.shape.y, 0.0)));\n"
    "    if (i.shape.y < 0.0) {\n"
    "        float f = saturate(1.0 - max(d, 0.0) / -i.shape.y);\n"
    "        cov = f * f;\n"
    "    }\n"
    "    return float4(i.color.rgb, i.color.a * t * cov);\n"
    "}\n";

static const char *MSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct params { float2 disp_size; float2 pad; };\n"
    "struct vs_in {\n"
    "    float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]];\n"
    "    float4 rect [[attribute(3)]]; float2 shape [[attribute(4)]];\n"
    "};\n"
    "struct vs_out { float4 pos [[position]]; float2 uv; float4 color; float4 rect; float2 shape; };\n"
    "vertex vs_out vs_main(vs_in i [[stage_in]], constant params& p [[buffer(0)]]) {\n"
    "    vs_out o;\n"
    "    o.pos = float4(i.pos / p.disp_size * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);\n"
    "    o.uv = i.uv; o.color = i.color; o.rect = i.rect; o.shape = i.shape;\n"
    "    return o;\n"
    "}\n"
    "static float sd_round_box(float2 p, float2 b, float r) {\n"
    "    float2 q = abs(p) - b + r;\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "fragment float4 fs_main(vs_out i [[stage_in]], texture2d<float> tex [[texture(0)]],\n"
    "                        sampler smp [[sampler(0)]]) {\n"
    "    float t = tex.sample(smp, i.uv).r;\n"
    "    if (i.uv.x < 0.0) t = 1.0;\n"
    "    float d = sd_round_box(i.rect.xy, i.rect.zw, i.shape.x);\n"
    "    float cov = saturate(0.5 - d);\n"
    "    if (i.shape.y > 0.0)\n"
    "        cov *= saturate(0.5 + sd_round_box(i.rect.xy, i.rect.zw - i.shape.y,\n"
    "                                           max(i.shape.x - i.shape.y, 0.0)));\n"
    "    if (i.shape.y < 0.0) {\n"
    "        float f = saturate(1.0 - max(d, 0.0) / -i.shape.y);\n"
    "        cov = f * f;\n"
    "    }\n"
    "    return float4(i.color.rgb, i.color.a * t * cov);\n"
    "}\n";

static sg_shader make_shader(void) {
    sg_shader_desc d = {0};
    if (sg_query_backend() == SG_BACKEND_D3D11) {
        d.vertex_func.source = HLSL_VS;
        d.fragment_func.source = HLSL_FS;
    } else {
        d.vertex_func.source = MSL;
        d.vertex_func.entry = "vs_main";
        d.fragment_func.source = MSL;
        d.fragment_func.entry = "fs_main";
    }
    for (int i = 0; i < 5; i++) {
        d.attrs[i].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[i].hlsl_sem_name = "TEXCOORD";
        d.attrs[i].hlsl_sem_index = (uint8_t)i;
    }
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(VsParams);
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.label = "clay-sokol-shader";
    return sg_make_shader(&d);
}

ClaySokol *clay_sokol_create(unsigned char *ttf) {
    ClaySokol *cs = calloc(1, sizeof(*cs));
    if (!cs)
        return NULL;
    cs->ttf = ttf;
    cs->scale = 1.0f;
    stbtt_InitFont(&cs->font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));

    cs->shd = make_shader();

    sg_pipeline_desc pd = {0};
    pd.shader = cs->shd;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[2].format = SG_VERTEXFORMAT_UBYTE4N;
    pd.layout.attrs[3].format = SG_VERTEXFORMAT_FLOAT4;
    pd.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT2;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label = "clay-sokol-pipeline";
    cs->pip = sg_make_pipeline(&pd);

    cs->vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = sizeof(cs->verts),
        .usage.write_transient = true,
        .label = "clay-sokol-vertices",
    });
    cs->smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

    // Bound for batches with no text; the shader ignores it for untextured quads.
    static const uint8_t white = 0xff;
    cs->white_img = sg_make_image(&(sg_image_desc){
        .width = 1,
        .height = 1,
        .pixel_format = SG_PIXELFORMAT_R8,
        .data.mip_levels[0] = {&white, 1},
    });
    cs->white_view = sg_make_view(&(sg_view_desc){.texture.image = cs->white_img});
    return cs;
}

static void drop_glyphs(ClaySokol *cs) {
    for (int i = 0; i < cs->num_glyphs; i++) {
        sg_destroy_view(cs->glyphs[i].view);
        sg_destroy_image(cs->glyphs[i].img);
    }
    cs->num_glyphs = 0;
}

void clay_sokol_destroy(ClaySokol *cs) {
    if (!cs)
        return;
    drop_glyphs(cs);
    sg_destroy_view(cs->white_view);
    sg_destroy_image(cs->white_img);
    sg_destroy_sampler(cs->smp);
    sg_destroy_buffer(cs->vbuf);
    sg_destroy_pipeline(cs->pip);
    sg_destroy_shader(cs->shd);
    free(cs->ttf);
    free(cs);
}

bool clay_sokol_set_scale(ClaySokol *cs, float scale) {
    if (scale == cs->scale)
        return false;
    cs->scale = scale;
    drop_glyphs(cs);
    return true;
}

// Bakes the font at this size on first use. The cache holds MAX_FONT_SIZES
// sizes; past that, the last slot is rebaked.
static Glyphs *get_glyphs(ClaySokol *cs, int font_size) {
    int px = (int)lroundf((float)font_size * cs->scale);
    if (px < 4)
        px = 4;
    for (int i = 0; i < cs->num_glyphs; i++)
        if (cs->glyphs[i].px == px)
            return &cs->glyphs[i];

    Glyphs *g;
    if (cs->num_glyphs < MAX_FONT_SIZES) {
        g = &cs->glyphs[cs->num_glyphs++];
    } else {
        g = &cs->glyphs[MAX_FONT_SIZES - 1];
        sg_destroy_view(g->view);
        sg_destroy_image(g->img);
    }
    memset(g, 0, sizeof(*g));
    g->px = px;

    uint8_t *bitmap = NULL;
    for (int dim = 128; dim <= 4096; dim *= 2) {
        bitmap = calloc((size_t)dim * dim, 1);
        if (stbtt_BakeFontBitmap(cs->ttf, stbtt_GetFontOffsetForIndex(cs->ttf, 0),
                                 (float)px, bitmap, dim, dim, FIRST_CHAR, NUM_CHARS,
                                 g->chars) > 0) {
            g->w = g->h = dim;
            break;
        }
        free(bitmap);
        bitmap = NULL;
    }

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&cs->font, &ascent, &descent, &line_gap);
    g->ascent = (float)ascent * stbtt_ScaleForPixelHeight(&cs->font, (float)px);

    if (bitmap) {
        g->img = sg_make_image(&(sg_image_desc){
            .width = g->w,
            .height = g->h,
            .pixel_format = SG_PIXELFORMAT_R8,
            .data.mip_levels[0] = {bitmap, (size_t)g->w * g->h},
            .label = "clay-sokol-glyphs",
        });
        g->view = sg_make_view(&(sg_view_desc){.texture.image = g->img});
        free(bitmap);
    } else {
        g->view = cs->white_view; // font too large to bake: text renders as blocks
    }
    return g;
}

static int char_index(char c) {
    unsigned char u = (unsigned char)c;
    if (u < FIRST_CHAR || u >= FIRST_CHAR + NUM_CHARS)
        u = '?';
    return u - FIRST_CHAR;
}

Clay_Dimensions clay_sokol_measure_text(Clay_StringSlice text,
                                        Clay_TextElementConfig *config,
                                        void *userData) {
    ClaySokol *cs = userData;
    Glyphs *g = get_glyphs(cs, config->fontSize);
    float width = 0.0f;
    for (int32_t i = 0; i < text.length; i++)
        width += g->chars[char_index(text.chars[i])].xadvance;
    width = width / cs->scale + (float)(config->letterSpacing * text.length);
    float height = config->lineHeight ? config->lineHeight : config->fontSize;
    return (Clay_Dimensions){width, height};
}

static uint32_t pack_color(Clay_Color c) {
    return (uint32_t)c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16 |
           (uint32_t)c.a << 24;
}

// An invalid view means "any": untextured quads join the open batch.
static bool use_batch(ClaySokol *cs, sg_view view) {
    Batch *b = cs->num_batches ? &cs->batches[cs->num_batches - 1] : NULL;
    bool same_clip = b && b->sx == cs->sx && b->sy == cs->sy && b->sw == cs->sw &&
                     b->sh == cs->sh;
    if (same_clip && (view.id == SG_INVALID_ID || view.id == b->view.id))
        return true;
    if (b && b->count == 0) {
        b->view = view.id == SG_INVALID_ID ? cs->white_view : view;
        b->sx = cs->sx, b->sy = cs->sy, b->sw = cs->sw, b->sh = cs->sh;
        return true;
    }
    if (cs->num_batches == MAX_BATCHES)
        return false;
    b = &cs->batches[cs->num_batches++];
    *b = (Batch){
        .first = cs->num_verts,
        .view = view.id == SG_INVALID_ID ? cs->white_view : view,
        .sx = cs->sx, .sy = cs->sy, .sw = cs->sw, .sh = cs->sh,
    };
    return true;
}

// Rounded box centered at (cx, cy) with half sizes (hu, hv) along the unit
// axis (ux, uy) and its perpendicular, in pixels. edge works as described at
// Vertex. The quad overhangs by 1px for the AA fringe, plus the glow width.
static void push_shape(ClaySokol *cs, float cx, float cy, float ux, float uy,
                       float hu, float hv, Clay_Color color, float radius,
                       float edge) {
    if (cs->num_verts + 6 > MAX_VERTS || !use_batch(cs, (sg_view){0}))
        return;
    static const float su[6] = {-1, 1, 1, -1, 1, -1};
    static const float sv[6] = {-1, -1, 1, -1, 1, 1};
    float r = fminf(radius, fminf(hu, hv));
    float pad = 1.0f + (edge < 0.0f ? -edge : 0.0f);
    uint32_t c = pack_color(color);
    for (int i = 0; i < 6; i++) {
        float lu = su[i] * (hu + pad), lv = sv[i] * (hv + pad);
        cs->verts[cs->num_verts++] = (Vertex){
            cx + ux * lu - uy * lv, cy + uy * lu + ux * lv, -1.0f, -1.0f, c,
            {lu, lv, hu, hv}, {r, edge},
        };
    }
    cs->batches[cs->num_batches - 1].count += 6;
}

static void push_rect(ClaySokol *cs, float x0, float y0, float x1, float y1,
                      Clay_Color color, float radius, float border) {
    push_shape(cs, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 1.0f, 0.0f,
               (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, color, radius, border);
}

// A capsule around the segment: the half length includes the round caps.
static void push_capsule(ClaySokol *cs, float x0, float y0, float x1, float y1,
                         float width, Clay_Color color, float edge) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float hv = width * 0.5f;
    float ux = len > 0.0f ? dx / len : 1.0f, uy = len > 0.0f ? dy / len : 0.0f;
    push_shape(cs, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, ux, uy, len * 0.5f + hv,
               hv, color, hv, edge);
}

void clay_sokol_line(ClaySokol *cs, float x0, float y0, float x1, float y1,
                     float width, Clay_Color color) {
    push_capsule(cs, x0, y0, x1, y1, width, color, 0.0f);
}

void clay_sokol_glow_line(ClaySokol *cs, float x0, float y0, float x1, float y1,
                          float width, float glow, Clay_Color color) {
    if (glow > 0.0f)
        push_capsule(cs, x0, y0, x1, y1, width, color, -glow);
}

static void push_glyph(ClaySokol *cs, const Glyphs *g, const stbtt_aligned_quad *q,
                       uint32_t c) {
    if (cs->num_verts + 6 > MAX_VERTS || !use_batch(cs, g->view))
        return;
    const float xs[6] = {q->x0, q->x1, q->x1, q->x0, q->x1, q->x0};
    const float ys[6] = {q->y0, q->y0, q->y1, q->y0, q->y1, q->y1};
    const float us[6] = {q->s0, q->s1, q->s1, q->s0, q->s1, q->s0};
    const float vs[6] = {q->t0, q->t0, q->t1, q->t0, q->t1, q->t1};
    for (int i = 0; i < 6; i++) {
        // huge half size: the SDF never clips glyphs
        cs->verts[cs->num_verts++] = (Vertex){
            xs[i], ys[i], us[i], vs[i], c, {0.0f, 0.0f, 1e5f, 1e5f}, {0.0f, 0.0f},
        };
    }
    cs->batches[cs->num_batches - 1].count += 6;
}

static void push_text(ClaySokol *cs, Clay_BoundingBox bb,
                      const Clay_TextRenderData *td) {
    Glyphs *g = get_glyphs(cs, td->fontSize);
    float s = cs->scale;
    float line_h = td->lineHeight ? td->lineHeight : td->fontSize;
    float x = bb.x * s;
    float y = bb.y * s + (line_h - td->fontSize) * 0.5f * s + g->ascent;
    uint32_t c = pack_color(td->textColor);
    for (int32_t i = 0; i < td->stringContents.length; i++) {
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g->chars, g->w, g->h,
                           char_index(td->stringContents.chars[i]), &x, &y, &q, 1);
        push_glyph(cs, g, &q, c);
        x += td->letterSpacing * s;
    }
}

static void set_scissor(ClaySokol *cs, int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    cs->sx = x < 0 ? 0 : x;
    cs->sy = y < 0 ? 0 : y;
    cs->sw = (x1 > cs->fb_width ? cs->fb_width : x1) - cs->sx;
    cs->sh = (y1 > cs->fb_height ? cs->fb_height : y1) - cs->sy;
    if (cs->sw < 0)
        cs->sw = 0;
    if (cs->sh < 0)
        cs->sh = 0;
}

void clay_sokol_prepare(ClaySokol *cs, Clay_RenderCommandArray cmds, int fb_width,
                        int fb_height, ClaySokolCustomFn custom, void *user) {
    cs->num_verts = 0;
    cs->num_batches = 0;
    cs->fb_width = fb_width;
    cs->fb_height = fb_height;
    set_scissor(cs, 0, 0, fb_width, fb_height);
    float s = cs->scale;

    for (int32_t i = 0; i < cmds.length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&cmds, i);
        Clay_BoundingBox bb = cmd->boundingBox;
        float x0 = bb.x * s, y0 = bb.y * s;
        float x1 = (bb.x + bb.width) * s, y1 = (bb.y + bb.height) * s;

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            const Clay_RectangleRenderData *r = &cmd->renderData.rectangle;
            push_rect(cs, x0, y0, x1, y1, r->backgroundColor,
                      r->cornerRadius.topLeft * s, 0.0f);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
            const Clay_CustomRenderData *cd = &cmd->renderData.custom;
            if (cd->backgroundColor.a > 0.0f)
                push_rect(cs, x0, y0, x1, y1, cd->backgroundColor,
                          cd->cornerRadius.topLeft * s, 0.0f);
            if (custom)
                custom(cs, (Clay_BoundingBox){x0, y0, x1 - x0, y1 - y0},
                       cd->customData, user);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            // uniform width only: the left width stands for all sides
            const Clay_BorderRenderData *b = &cmd->renderData.border;
            if (b->width.left > 0)
                push_rect(cs, x0, y0, x1, y1, b->color, b->cornerRadius.topLeft * s,
                          b->width.left * s);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            push_text(cs, bb, &cmd->renderData.text);
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            set_scissor(cs, (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1 - x0),
                        (int)ceilf(y1 - y0));
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            set_scissor(cs, 0, 0, fb_width, fb_height);
            break;
        default: // images and overlays are not supported
            break;
        }
    }

    if (cs->num_verts > 0) {
        sg_write_buffer_transient(&(sg_write_buffer_desc){
            .src.data = {cs->verts, (size_t)cs->num_verts * sizeof(Vertex)},
            .dst.buffer = cs->vbuf,
        });
    }
}

void clay_sokol_draw(ClaySokol *cs) {
    if (cs->num_verts == 0)
        return;
    sg_apply_viewport(0, 0, cs->fb_width, cs->fb_height, true);
    sg_apply_pipeline(cs->pip);
    VsParams params = {{(float)cs->fb_width, (float)cs->fb_height}, {0, 0}};
    sg_apply_uniforms(0, SG_RANGE_REF(params));
    for (int i = 0; i < cs->num_batches; i++) {
        const Batch *b = &cs->batches[i];
        if (b->count == 0 || b->sw == 0 || b->sh == 0)
            continue;
        sg_apply_scissor_rect(b->sx, b->sy, b->sw, b->sh, true);
        sg_bindings bind = {0};
        bind.vertex_buffers[0] = cs->vbuf;
        bind.views[0] = b->view;
        bind.samplers[0] = cs->smp;
        sg_apply_bindings(&bind);
        sg_draw(b->first, b->count, 1);
    }
    sg_apply_scissor_rect(0, 0, cs->fb_width, cs->fb_height, true);
}
