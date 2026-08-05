// overlay.c — on-screen gamepad: SDF-drawn, mm-sized, manifest-driven.
//
// Semantics adopted from RetroArch's decade of overlay tuning (see the plan
// doc): hitboxes bigger than visuals (reach), hitboxes that grow while held
// (range_mod), an eightway dpad area resolved by angle so directions glide
// without lifting, and roll-off between face buttons (PPSSPP's gliding).
// Layout is computed from the letterbox rect so bezel bars get used first,
// and everything is sized in millimeters — pixel-based layouts are the root
// of RetroArch's Android misalignment bugs.

#include "overlay.h"
#include <GLES3/gl3.h>
#include <math.h>
#include <string.h>

// ─── Tunables (mm unless noted) ─────────────────────────────────────────────

#define DIAL_R_MM        16.0f  // dpad/stick dial radius
#define FACE_R4_MM        7.0f  // face button radius when 3-4 buttons
#define FACE_R2_MM        8.5f  // face button radius when 1-2 buttons
// ring radii keep adjacent buttons (ring*sqrt2 apart) from overlapping
#define FACE_RING4_MM    11.0f  // diamond ring radius (3-4 buttons)
#define FACE_RING2_MM    13.5f  // ring for 1-2 buttons
#define SHOULDER_W_MM    16.0f
#define SHOULDER_H_MM     8.0f
#define TRIGGER_W_MM     14.0f
#define TRIGGER_H_MM      7.0f
#define PILL_W_MM        12.0f
#define PILL_H_MM         5.0f
#define EDGE_INSET_MM     6.0f  // keeps clear of display cutouts in landscape
#define STICK_THROW_MM   12.0f
#define STICK_SAT         0.80f // saturate at 80% throw (PPSSPP convention)

#define REACH             1.30f // hitbox = visual * reach
#define RANGE_MOD         1.50f // hitbox growth while pressed
#define DIAG_WINDOW_DEG  40.0f  // diagonal sector width (cardinals get the rest)
#define DPAD_DEADZONE     0.14f // fraction of dial radius
#define BASE_ALPHA        0.35f
#define DIM_ALPHA         0.15f
#define IDLE_DIM_MS    8000.0
#define FADE_MS         200.0
#define DIM_FADE_MS     300.0

// ─── Controls ───────────────────────────────────────────────────────────────

enum { K_DPAD, K_STICK, K_BUTTON, K_RECT };  // K_RECT: shoulders/triggers/pills

typedef struct {
    int      kind;
    uint16_t bits;        // WC_BUTTON_* set while pressed (buttons/rects)
    uint8_t  trigger;     // 1 = left trigger, 2 = right (writes 255, not bits)
    float    cx, cy;      // center, window px
    float    hx, hy;      // half extents, px (hx==hy==radius for round kinds)
    int      corner;      // rect hit area extends into this screen corner:
                          // 0 none, 1 top-left, 2 top-right
    float    r, g, b;
    bool     pressed;
    float    press_anim;  // 0..1 smoothed
} ctl_t;

#define MAX_CTLS 12
static ctl_t ctls[MAX_CTLS];
static int   n_ctls = 0;

typedef struct {
    SDL_FingerID id;
    bool  used;
    int   ctl;            // index into ctls
    float ox, oy;         // stick: floating origin (window px)
} touch_t;
static touch_t touches[10];

static uint32_t g_mask = 0;
static float    g_ppmm = 16.5f;      // ~420dpi fallback
static int      g_win_w = 0, g_win_h = 0;

// dpad/stick live state
static uint16_t dpad_bits = 0;
static float    stick_x = 0.0f, stick_y = 0.0f; // -1..1
static int      dpad_ctl = -1;                  // index of the left dial

// visibility + fades
static bool   g_visible = false;
static float  g_fade = 0.0f;         // 0..1
static float  g_dim = 1.0f;          // idle dim factor
static double g_last_touch_ms = 0.0;
static double g_last_render_ms = 0.0;

// GL
static GLuint prog = 0, vao = 0;
static GLint  u_screen, u_center, u_half, u_shape, u_color, u_dirbits, u_param;
enum { SH_CIRCLE = 0, SH_DPAD = 1, SH_RRECT = 2, SH_RING = 3 };

// haptics
static SDL_Haptic* haptic = NULL;

// ─── Haptics ────────────────────────────────────────────────────────────────

static void haptic_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) return;
    // SDL's Android backend registers the system vibrator as a haptic device
    if (SDL_NumHaptics() > 0) {
        haptic = SDL_HapticOpen(0);
        if (haptic && SDL_HapticRumbleInit(haptic) != 0) {
            SDL_HapticClose(haptic);
            haptic = NULL;
        }
    }
}

static void haptic_pulse(float strength, uint32_t ms) {
    if (haptic) SDL_HapticRumblePlay(haptic, strength, ms);
}

// ─── Layout ────────────────────────────────────────────────────────────────

static float mm(float v) { return v * g_ppmm; }

static ctl_t* add_ctl(int kind, uint16_t bits, float cx, float cy,
                      float hx, float hy, float r, float g, float b) {
    if (n_ctls >= MAX_CTLS) return NULL;
    ctl_t* c = &ctls[n_ctls++];
    memset(c, 0, sizeof(*c));
    c->kind = kind; c->bits = bits;
    c->cx = cx; c->cy = cy; c->hx = hx; c->hy = hy;
    c->r = r; c->g = g; c->b = b;
    return c;
}

void overlay_layout(int win_w, int win_h,
                    float game_x, float game_y, float game_w, float game_h) {
    (void)game_y; (void)game_h;
    g_win_w = win_w; g_win_h = win_h;

    // preserve pressed state across relayout (rare: rotation/cart resize)
    n_ctls = 0;
    memset(touches, 0, sizeof(touches));
    dpad_bits = 0; stick_x = stick_y = 0.0f; dpad_ctl = -1;

    float inset = mm(EDGE_INSET_MM);
    float bar_l = game_x;                          // left bezel bar width
    float bar_r = win_w - (game_x + game_w);       // right bar width

    bool has_shoulders = (g_mask & (WC_CTRL_L | WC_CTRL_R)) != 0;
    bool has_triggers  = (g_mask & (WC_CTRL_LTRIG | WC_CTRL_RTRIG)) != 0;
    int  face_n = !!(g_mask & WC_CTRL_A) + !!(g_mask & WC_CTRL_B) +
                  !!(g_mask & WC_CTRL_X) + !!(g_mask & WC_CTRL_Y);

    // fewer controls -> bigger targets (minimal games get comfortable ones)
    float scale = (!has_shoulders && !has_triggers && face_n <= 2) ? 1.15f : 1.0f;

    // The bezel rule: when the letterbox bars are wide enough (4:3-ish carts
    // on tall phones), shrink both clusters just enough to sit FULLY inside
    // the bars — zero game pixels covered. Narrow bars (16:9 carts) keep
    // ideal sizes and overlap the game edge translucently instead.
    float ring_ideal = mm(face_n >= 3 ? FACE_RING4_MM : FACE_RING2_MM) * scale;
    float br_ideal   = mm(face_n >= 3 ? FACE_R4_MM : FACE_R2_MM) * scale;
    float r_ideal    = mm(DIAL_R_MM) * scale;
    float bar        = fminf(bar_l, bar_r);
    float avail      = bar * 0.5f - mm(1.5f);
    float ext_ideal  = fmaxf(r_ideal, ring_ideal + br_ideal);
    bool  bezel_mode = avail >= mm(11.5f);
    float fit = bezel_mode ? fmaxf(0.65f, fminf(1.0f, avail / ext_ideal)) : 1.0f;
    float dial_r = r_ideal * fit;
    float ring   = ring_ideal * fit;
    float br     = br_ideal * fit;
    float edge_inset = bezel_mode ? mm(3.0f) : inset;

    float dial_y = fminf(win_h * 0.62f, win_h - dial_r - inset);

    // Left dial: dpad and/or left stick, centered in the bar when possible.
    if (g_mask & (WC_CTRL_DPAD | WC_CTRL_LSTICK)) {
        float cx = fmaxf(bar_l * 0.5f, edge_inset + dial_r);
        int kind = (g_mask & WC_CTRL_LSTICK) ? K_STICK : K_DPAD;
        ctl_t* c = add_ctl(kind, 0, cx, dial_y, dial_r, dial_r, 0.85f, 0.85f, 0.88f);
        if (c) dpad_ctl = (int)(c - ctls);
    }

    // Right dial: face buttons at their Xbox diamond anchors (A bottom,
    // B right, X left, Y top) so muscle memory holds for any subset.
    if (face_n > 0) {
        float cx = win_w - fmaxf(bar_r * 0.5f, edge_inset + ring + br);
        if (g_mask & WC_CTRL_A)
            add_ctl(K_BUTTON, WC_BUTTON_A, cx, dial_y + ring, br, br, 0.30f, 0.69f, 0.31f);
        if (g_mask & WC_CTRL_B)
            add_ctl(K_BUTTON, WC_BUTTON_B, cx + ring, dial_y, br, br, 0.90f, 0.22f, 0.21f);
        if (g_mask & WC_CTRL_X)
            add_ctl(K_BUTTON, WC_BUTTON_X, cx - ring, dial_y, br, br, 0.26f, 0.45f, 0.91f);
        if (g_mask & WC_CTRL_Y)
            add_ctl(K_BUTTON, WC_BUTTON_Y, cx, dial_y - ring, br, br, 0.95f, 0.77f, 0.06f);
    }

    // Shoulders: top corners, hitboxes reaching into the very corner
    float sw = mm(SHOULDER_W_MM) * 0.5f, sh = mm(SHOULDER_H_MM) * 0.5f;
    if (g_mask & WC_CTRL_L) {
        ctl_t* c = add_ctl(K_RECT, WC_BUTTON_L, inset + sw, inset + sh, sw, sh,
                           0.85f, 0.85f, 0.88f);
        if (c) c->corner = 1;
    }
    if (g_mask & WC_CTRL_R) {
        ctl_t* c = add_ctl(K_RECT, WC_BUTTON_R, win_w - inset - sw, inset + sh,
                           sw, sh, 0.85f, 0.85f, 0.88f);
        if (c) c->corner = 2;
    }

    // Triggers: below the shoulders, binary 0/255 (screens have no pressure)
    float tw = mm(TRIGGER_W_MM) * 0.5f, th = mm(TRIGGER_H_MM) * 0.5f;
    float trig_y = inset + sh * 2.0f + mm(3.0f) + th;
    if (g_mask & WC_CTRL_LTRIG) {
        ctl_t* c = add_ctl(K_RECT, 0, inset + tw, trig_y, tw, th, 0.75f, 0.75f, 0.80f);
        if (c) c->trigger = 1;
    }
    if (g_mask & WC_CTRL_RTRIG) {
        ctl_t* c = add_ctl(K_RECT, 0, win_w - inset - tw, trig_y, tw, th,
                           0.75f, 0.75f, 0.80f);
        if (c) c->trigger = 2;
    }

    // Start/Select: pills at bottom-center (sticky immersive means taps here
    // don't fight the home gesture, which needs a swipe)
    float pw = mm(PILL_W_MM) * 0.5f, ph = mm(PILL_H_MM) * 0.5f;
    float pill_y = win_h - inset - ph;
    bool sel = (g_mask & WC_CTRL_SELECT) != 0, sta = (g_mask & WC_CTRL_START) != 0;
    if (sel && sta) {
        add_ctl(K_RECT, WC_BUTTON_SELECT, win_w * 0.5f - pw - mm(3.0f), pill_y,
                pw, ph, 0.75f, 0.75f, 0.80f);
        add_ctl(K_RECT, WC_BUTTON_START, win_w * 0.5f + pw + mm(3.0f), pill_y,
                pw, ph, 0.75f, 0.75f, 0.80f);
    } else if (sel || sta) {
        add_ctl(K_RECT, sta ? WC_BUTTON_START : WC_BUTTON_SELECT,
                win_w * 0.5f, pill_y, pw, ph, 0.75f, 0.75f, 0.80f);
    }
}

// ─── Hit testing ────────────────────────────────────────────────────────────

static bool ctl_hit(const ctl_t* c, float px, float py) {
    float m = REACH * (c->pressed ? RANGE_MOD : 1.0f);
    if (c->kind == K_RECT) {
        // shoulders own their whole screen corner (reach_up/left semantics)
        float x0 = c->cx - c->hx * m, x1 = c->cx + c->hx * m;
        float y0 = c->cy - c->hy * m, y1 = c->cy + c->hy * m;
        if (c->corner == 1) { x0 = 0; y0 = 0; }
        if (c->corner == 2) { x1 = (float)g_win_w; y0 = 0; }
        return px >= x0 && px <= x1 && py >= y0 && py <= y1;
    }
    float dx = (px - c->cx) / (c->hx * m);
    float dy = (py - c->cy) / (c->hy * m);
    return dx * dx + dy * dy <= 1.0f;
}

// Reach-grown hitboxes overlap (close face buttons especially), so a touch
// belongs to the NEAREST control that contains it, not the first in the array.
static int find_ctl(float px, float py) {
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < n_ctls; i++) {
        if (!ctl_hit(&ctls[i], px, py)) continue;
        float dx = px - ctls[i].cx, dy = py - ctls[i].cy;
        float d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Roll-off target: nearest face button containing the point at BASE reach
// (ignoring range_mod growth — otherwise a grown hitbox swallows its
// neighbors and sliding between close buttons can never transfer).
static int find_button_base(float px, float py) {
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < n_ctls; i++) {
        ctl_t* c = &ctls[i];
        if (c->kind != K_BUTTON) continue;
        float dx = (px - c->cx) / (c->hx * REACH);
        float dy = (py - c->cy) / (c->hy * REACH);
        if (dx * dx + dy * dy > 1.0f) continue;
        float d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static touch_t* touch_find(SDL_FingerID id) {
    for (int i = 0; i < 10; i++)
        if (touches[i].used && touches[i].id == id) return &touches[i];
    return NULL;
}

static touch_t* touch_alloc(SDL_FingerID id) {
    touch_t* t = touch_find(id);
    if (t) return t;
    for (int i = 0; i < 10; i++) {
        if (!touches[i].used) {
            touches[i].used = true;
            touches[i].id = id;
            touches[i].ctl = -1;
            return &touches[i];
        }
    }
    return NULL;
}

// Eightway: direction from angle, diagonals get DIAG_WINDOW_DEG-wide sectors.
// (RetroArch dpad_area semantics; glide between directions without lifting.)
static uint16_t eightway_bits(float dx, float dy, float radius) {
    float len = sqrtf(dx * dx + dy * dy);
    if (len < radius * DPAD_DEADZONE) return 0;
    float ang = atan2f(-dy, dx) * (180.0f / (float)M_PI); // y-down -> y-up
    if (ang < 0) ang += 360.0f;

    const float dw = DIAG_WINDOW_DEG * 0.5f;
    // diagonal centers 45/135/225/315
    if (fabsf(ang -  45.0f) <= dw) return WC_BUTTON_UP | WC_BUTTON_RIGHT;
    if (fabsf(ang - 135.0f) <= dw) return WC_BUTTON_UP | WC_BUTTON_LEFT;
    if (fabsf(ang - 225.0f) <= dw) return WC_BUTTON_DOWN | WC_BUTTON_LEFT;
    if (fabsf(ang - 315.0f) <= dw) return WC_BUTTON_DOWN | WC_BUTTON_RIGHT;
    if (ang >= 315.0f + dw || ang < 45.0f - dw)   return WC_BUTTON_RIGHT;
    if (ang < 135.0f - dw)                        return WC_BUTTON_UP;
    if (ang < 225.0f - dw)                        return WC_BUTTON_LEFT;
    return WC_BUTTON_DOWN;
}

static void press_ctl(int idx) {
    ctl_t* c = &ctls[idx];
    if (!c->pressed) {
        c->pressed = true;
        haptic_pulse(0.55f, 14);
    }
}

static void release_ctl(int idx) {
    if (idx < 0 || idx >= n_ctls) return;
    ctl_t* c = &ctls[idx];
    c->pressed = false;
    if (c->kind == K_DPAD || c->kind == K_STICK) {
        dpad_bits = 0;
        stick_x = stick_y = 0.0f;
    }
}

static void dial_update(touch_t* t, float px, float py) {
    ctl_t* c = &ctls[t->ctl];
    if (c->kind == K_DPAD) {
        uint16_t nb = eightway_bits(px - c->cx, py - c->cy, c->hx);
        if (nb != dpad_bits && nb != 0) haptic_pulse(0.35f, 9);
        dpad_bits = nb;
    } else { // K_STICK: floating origin at touch-down, saturating throw
        float throw_px = mm(STICK_THROW_MM) * STICK_SAT;
        float ax = (px - t->ox) / throw_px;
        float ay = (py - t->oy) / throw_px;
        float len = sqrtf(ax * ax + ay * ay);
        if (len > 1.0f) { ax /= len; ay /= len; }
        stick_x = ax; stick_y = ay;
        // stick also drives dpad bits when the cart declared both
        if (g_mask & WC_CTRL_DPAD) {
            uint16_t nb = 0;
            if (ay < -0.5f) nb |= WC_BUTTON_UP;
            if (ay >  0.5f) nb |= WC_BUTTON_DOWN;
            if (ax < -0.5f) nb |= WC_BUTTON_LEFT;
            if (ax >  0.5f) nb |= WC_BUTTON_RIGHT;
            dpad_bits = nb;
        }
    }
}

bool overlay_event(const SDL_Event* ev, int win_w, int win_h, double now_ms) {
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION &&
        ev->type != SDL_FINGERUP)
        return false;
    if (!g_visible) return true; // still consume: cart is not a pointer cart

    float px = ev->tfinger.x * (float)win_w;
    float py = ev->tfinger.y * (float)win_h;

    if (ev->type == SDL_FINGERDOWN) {
        int idx = find_ctl(px, py);
        if (idx < 0) return true; // touched nothing: ignored, by design
        touch_t* t = touch_alloc(ev->tfinger.fingerId);
        if (!t) return true;
        t->ctl = idx;
        g_last_touch_ms = now_ms;
        ctl_t* c = &ctls[idx];
        if (c->kind == K_STICK) {
            t->ox = px; t->oy = py;
            c->pressed = true;
            haptic_pulse(0.35f, 9);
            dial_update(t, px, py);
        } else if (c->kind == K_DPAD) {
            c->pressed = true;
            haptic_pulse(0.35f, 9);
            dial_update(t, px, py);
        } else {
            press_ctl(idx);
        }
        return true;
    }

    touch_t* t = touch_find(ev->tfinger.fingerId);
    if (!t || t->ctl < 0) return true;
    g_last_touch_ms = now_ms;

    if (ev->type == SDL_FINGERMOTION) {
        ctl_t* c = &ctls[t->ctl];
        if (c->kind == K_DPAD || c->kind == K_STICK) {
            dial_update(t, px, py);
        } else if (c->kind == K_BUTTON) {
            // roll-off: sliding from one face button onto a neighbor presses
            // the neighbor and releases this one (PPSSPP touch gliding)
            int idx = find_button_base(px, py);
            if (idx >= 0 && idx != t->ctl) {
                release_ctl(t->ctl);
                t->ctl = idx;
                press_ctl(idx);
            }
            // off everything: keep held (range_mod semantics; dropping input
            // on a drifting thumb is the worse failure)
        }
        return true;
    }

    // SDL_FINGERUP
    release_ctl(t->ctl);
    t->used = false;
    return true;
}

// ─── Pad merge ──────────────────────────────────────────────────────────────

void overlay_apply(wc_pad_t* pad) {
    if (!g_visible) return;
    pad->connected = 1;
    pad->buttons |= dpad_bits;
    for (int i = 0; i < n_ctls; i++) {
        if (!ctls[i].pressed) continue;
        pad->buttons |= ctls[i].bits;
        if (ctls[i].trigger == 1) pad->left_trigger = 255;
        if (ctls[i].trigger == 2) pad->right_trigger = 255;
    }
    if ((g_mask & WC_CTRL_LSTICK) && (stick_x != 0.0f || stick_y != 0.0f)) {
        pad->left_x = (int16_t)(stick_x * 32767.0f);
        pad->left_y = (int16_t)(stick_y * 32767.0f);
    }
}

// ─── Rendering ──────────────────────────────────────────────────────────────

static const char* VS =
    "#version 300 es\n"
    "uniform vec2 u_screen, u_center, u_half;\n"
    "out vec2 v_local;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1) * 2.0 - 1.0,\n"
    "                float((gl_VertexID >> 1) & 1) * 2.0 - 1.0);\n"
    "  v_local = c;\n"
    "  vec2 px = u_center + c * u_half;\n"
    "  vec2 clip = px / u_screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);\n"
    "}\n";

static const char* FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec2 u_half;\n"
    "uniform int u_shape, u_dirbits;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_param;\n"
    "in vec2 v_local;\n"
    "out vec4 frag;\n"
    "float sdBox(vec2 p, vec2 b) {\n"
    "  vec2 d = abs(p) - b;\n"
    "  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 p = v_local * u_half;\n"
    "  float r = min(u_half.x, u_half.y) - 2.0;\n"
    "  float sd;\n"
    "  float boost = 0.0;\n"
    "  if (u_shape == 0) {            // circle\n"
    "    sd = length(p) - r;\n"
    "  } else if (u_shape == 1) {     // dpad: base disc + cross glyph\n"
    "    float base = length(p) - r;\n"
    "    float w = r * 0.30, al = r * 0.92;\n"
    "    float cross = min(sdBox(p, vec2(al, w)), sdBox(p, vec2(w, al)));\n"
    "    // brighten the held arm(s): bits UP=256 DOWN=512 LEFT=1024 RIGHT=2048\n"
    "    if ((u_dirbits & 256)  != 0 && p.y < -w) boost = 0.5;\n"
    "    if ((u_dirbits & 512)  != 0 && p.y >  w) boost = 0.5;\n"
    "    if ((u_dirbits & 1024) != 0 && p.x < -w) boost = 0.5;\n"
    "    if ((u_dirbits & 2048) != 0 && p.x >  w) boost = 0.5;\n"
    "    float ba = 1.0 - smoothstep(-1.5, 1.5, base);\n"
    "    float ca = 1.0 - smoothstep(-1.5, 1.5, cross);\n"
    "    frag = vec4(u_color.rgb * (1.0 + boost),\n"
    "                max(ba * u_color.a * 0.35, ca * u_color.a));\n"
    "    return;\n"
    "  } else if (u_shape == 2) {     // rounded rect\n"
    "    sd = sdBox(p, u_half - vec2(2.0 + u_param)) - u_param;\n"
    "  } else {                       // ring (stick base)\n"
    "    sd = abs(length(p) - r * 0.82) - r * 0.10;\n"
    "  }\n"
    "  float fill = 1.0 - smoothstep(-1.5, 1.5, sd);\n"
    "  float rim = 1.0 - smoothstep(0.0, 2.5, abs(sd));\n"
    "  float a = max(fill * u_color.a, rim * min(1.0, u_color.a * 1.9));\n"
    "  frag = vec4(u_color.rgb, a);\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

void overlay_init(uint32_t controls_mask, float px_per_mm) {
    g_mask = controls_mask;
    if (px_per_mm > 1.0f) g_ppmm = px_per_mm;

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    u_screen  = glGetUniformLocation(prog, "u_screen");
    u_center  = glGetUniformLocation(prog, "u_center");
    u_half    = glGetUniformLocation(prog, "u_half");
    u_shape   = glGetUniformLocation(prog, "u_shape");
    u_color   = glGetUniformLocation(prog, "u_color");
    u_dirbits = glGetUniformLocation(prog, "u_dirbits");
    u_param   = glGetUniformLocation(prog, "u_param");
    glGenVertexArrays(1, &vao);

    haptic_init();
}

void overlay_set_visible(bool visible) { g_visible = visible; }

static void draw_ctl(float cx, float cy, float hx, float hy, int shape,
                     float r, float g, float b, float a, int dirbits, float param) {
    glUniform2f(u_center, cx, cy);
    glUniform2f(u_half, hx, hy);
    glUniform1i(u_shape, shape);
    glUniform4f(u_color, r, g, b, a);
    glUniform1i(u_dirbits, dirbits);
    glUniform1f(u_param, param);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void overlay_render(bool save_gl_state, double now_ms) {
    // advance fades even while invisible so re-show starts from reality
    float dt = (g_last_render_ms > 0.0) ? (float)(now_ms - g_last_render_ms) : 16.7f;
    g_last_render_ms = now_ms;
    if (dt > 100.0f) dt = 100.0f;

    float fade_target = g_visible ? 1.0f : 0.0f;
    g_fade += (fade_target - g_fade) * fminf(1.0f, dt / (float)FADE_MS);
    float dim_target =
        (now_ms - g_last_touch_ms > IDLE_DIM_MS) ? (DIM_ALPHA / BASE_ALPHA) : 1.0f;
    g_dim += (dim_target - g_dim) * fminf(1.0f, dt / (float)DIM_FADE_MS);

    float alpha = BASE_ALPHA * g_fade * g_dim;
    if (alpha < 0.01f) return;

    // GL carts keep internal state caches (Ganesh, gl4es); save what we touch
    GLint prev_prog = 0, prev_vao = 0;
    GLint prev_src_rgb = 0, prev_dst_rgb = 0, prev_src_a = 0, prev_dst_a = 0;
    GLint prev_vp[4] = {0};
    GLboolean prev_blend_on = GL_FALSE, prev_depth_on = GL_FALSE,
              prev_scissor_on = GL_FALSE;
    if (save_gl_state) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        glGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);
        glGetIntegerv(GL_VIEWPORT, prev_vp);
        prev_blend_on = glIsEnabled(GL_BLEND);
        prev_depth_on = glIsEnabled(GL_DEPTH_TEST);
        prev_scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_win_w, g_win_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniform2f(u_screen, (float)g_win_w, (float)g_win_h);

    for (int i = 0; i < n_ctls; i++) {
        ctl_t* c = &ctls[i];
        float target = c->pressed ? 1.0f : 0.0f;
        c->press_anim += (target - c->press_anim) * fminf(1.0f, dt / 60.0f);
        float a = alpha * (1.0f + 1.6f * c->press_anim);
        if (a > 0.95f) a = 0.95f;
        float grow = 1.0f + 0.06f * c->press_anim;

        if (c->kind == K_DPAD) {
            draw_ctl(c->cx, c->cy, c->hx * grow, c->hy * grow, SH_DPAD,
                     c->r, c->g, c->b, a, dpad_bits, 0.0f);
        } else if (c->kind == K_STICK) {
            draw_ctl(c->cx, c->cy, c->hx, c->hy, SH_RING, c->r, c->g, c->b, a, 0, 0.0f);
            float knob_r = c->hx * 0.42f;
            float throw_px = mm(STICK_THROW_MM);
            draw_ctl(c->cx + stick_x * throw_px, c->cy + stick_y * throw_px,
                     knob_r, knob_r, SH_CIRCLE, c->r, c->g, c->b,
                     fminf(0.95f, a * 1.5f), 0, 0.0f);
        } else if (c->kind == K_BUTTON) {
            draw_ctl(c->cx, c->cy, c->hx * grow, c->hy * grow, SH_CIRCLE,
                     c->r, c->g, c->b, a, 0, 0.0f);
        } else {
            draw_ctl(c->cx, c->cy, c->hx * grow, c->hy * grow, SH_RRECT,
                     c->r, c->g, c->b, a, 0, c->hy * 0.6f);
        }
    }

    if (save_gl_state) {
        glUseProgram((GLuint)prev_prog);
        glBindVertexArray((GLuint)prev_vao);
        if (!prev_blend_on) glDisable(GL_BLEND);
        glBlendFuncSeparate((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb,
                            (GLenum)prev_src_a, (GLenum)prev_dst_a);
        if (prev_depth_on) glEnable(GL_DEPTH_TEST);
        if (prev_scissor_on) glEnable(GL_SCISSOR_TEST);
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    }
}

void overlay_shutdown(void) {
    if (haptic) { SDL_HapticClose(haptic); haptic = NULL; }
    if (prog) { glDeleteProgram(prog); prog = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
}
