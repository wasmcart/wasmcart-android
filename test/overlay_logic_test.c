// overlay_logic_test.c — desktop test of the overlay's pure logic: eightway
// direction resolution, mm-based layout against letterbox bars, and the
// finger-event → pad-state flow. No window needed; GL calls never run
// because overlay_init/overlay_render are never called (state is driven
// directly, which is why this file #includes overlay.c for its statics).
//
// Build on a desktop with SDL2 + GLES headers:
//   gcc -O0 -o overlay_logic_test test/overlay_logic_test.c \
//       -Inative -Ideps/wasmcart-native/include \
//       $(sdl2-config --cflags --libs) -lGLESv2 -lm

// overlay.c logs shader/link failures through wc_log.h, whose backing globals
// normally live in the wasmcart-native library this standalone test does not
// link. Define them here so the documented build above stays a single gcc line.
#include <stdio.h>
FILE* _wc_log_file = NULL;
long  _wc_log_bytes = 0;

#include "../native/overlay.c"

static int failures = 0;
static void expect(const char* what, int cond) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static SDL_Event finger(Uint32 type, SDL_FingerID id, float nx, float ny) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.tfinger.fingerId = id;
    e.tfinger.x = nx;
    e.tfinger.y = ny;
    return e;
}

int main(void) {
    // 20:9 phone, 1080p class, ~428 dpi
    const int W = 2400, H = 1080;
    g_ppmm = 428.0f / 25.4f;

    // ── eightway ──
    expect("dead zone is neutral", eightway_bits(1.0f, 1.0f, 270.0f) == 0);
    expect("right", eightway_bits(200.0f, 0.0f, 270.0f) == WC_BUTTON_RIGHT);
    expect("up (y-down coords)", eightway_bits(0.0f, -200.0f, 270.0f) == WC_BUTTON_UP);
    expect("down", eightway_bits(0.0f, 200.0f, 270.0f) == WC_BUTTON_DOWN);
    expect("left", eightway_bits(-200.0f, 0.0f, 270.0f) == WC_BUTTON_LEFT);
    expect("up-right diagonal", eightway_bits(150.0f, -150.0f, 270.0f) ==
           (WC_BUTTON_UP | WC_BUTTON_RIGHT));
    expect("down-left diagonal", eightway_bits(-150.0f, 150.0f, 270.0f) ==
           (WC_BUTTON_DOWN | WC_BUTTON_LEFT));
    // diagonal window is 40 degrees (25-65), so 20 degrees is cardinal
    expect("20deg is cardinal right",
           eightway_bits(200.0f, -73.0f, 270.0f) == WC_BUTTON_RIGHT);
    // and 30 degrees IS inside the diagonal window, by design
    expect("30deg is diagonal", eightway_bits(200.0f, -115.0f, 270.0f) ==
           (WC_BUTTON_UP | WC_BUTTON_RIGHT));

    // ── layout: 4:3 cart on a 20:9 phone -> dials fully inside the bars ──
    g_mask = WC_CTRL_DEFAULT_SET;
    float game_w = H * 4.0f / 3.0f;               // 1440
    float game_x = (W - game_w) * 0.5f;           // 480px bars
    overlay_layout(W, H, game_x, 0, game_w, (float)H);
    expect("default set control count (dpad+4 face+2 shoulder+2 pill)", n_ctls == 9);
    expect("dpad exists", dpad_ctl >= 0 && ctls[dpad_ctl].kind == K_DPAD);
    {
        ctl_t* d = &ctls[dpad_ctl];
        expect("dpad centered in left bar", fabsf(d->cx - game_x * 0.5f) < mm(2.0f));
        expect("dpad fully inside bar", d->cx + d->hx <= game_x + 0.5f);
        // the whole point of bezel mode: nothing overlaps the game
        for (int i = 0; i < n_ctls; i++) {
            if (ctls[i].bits == WC_BUTTON_START || ctls[i].bits == WC_BUTTON_SELECT)
                continue; // pills are bottom-center over the game by design
            bool in_left  = ctls[i].cx + ctls[i].hx <= game_x + 0.5f;
            bool in_right = ctls[i].cx - ctls[i].hx >= game_x + game_w - 0.5f;
            expect("control fully in a bar (bezel mode)", in_left || in_right);
        }
    }

    // ── layout: minimal two-button game -> fewer, bigger controls ──
    g_mask = WC_CTRL_DPAD | WC_CTRL_A | WC_CTRL_B | WC_CTRL_START;
    overlay_layout(W, H, game_x, 0, game_w, (float)H);
    expect("minimal set count (dpad+a+b+start)", n_ctls == 4);
    float grown_r = ctls[dpad_ctl].hx;
    g_mask = WC_CTRL_DEFAULT_SET;
    overlay_layout(W, H, game_x, 0, game_w, (float)H);
    expect("minimal sets get bigger dials", grown_r > ctls[dpad_ctl].hx);

    // ── event flow: touch A -> pad bit; lift -> released ──
    g_mask = WC_CTRL_DPAD | WC_CTRL_A | WC_CTRL_B;
    overlay_layout(W, H, game_x, 0, game_w, (float)H);
    g_visible = true;
    int a_idx = -1, b_idx = -1;
    for (int i = 0; i < n_ctls; i++) {
        if (ctls[i].bits == WC_BUTTON_A) a_idx = i;
        if (ctls[i].bits == WC_BUTTON_B) b_idx = i;
    }
    expect("A and B exist", a_idx >= 0 && b_idx >= 0);

    SDL_Event e = finger(SDL_FINGERDOWN, 7,
                         ctls[a_idx].cx / W, ctls[a_idx].cy / H);
    expect("finger event consumed", overlay_event(&e, W, H, 1000.0));
    wc_pad_t pad;
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("A pressed", (pad.buttons & WC_BUTTON_A) != 0);
    expect("pad marked connected", pad.connected == 1);

    // roll-off: slide the same finger onto B
    e = finger(SDL_FINGERMOTION, 7, ctls[b_idx].cx / W, ctls[b_idx].cy / H);
    overlay_event(&e, W, H, 1100.0);
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("roll-off pressed B", (pad.buttons & WC_BUTTON_B) != 0);
    expect("roll-off released A", (pad.buttons & WC_BUTTON_A) == 0);

    e = finger(SDL_FINGERUP, 7, ctls[b_idx].cx / W, ctls[b_idx].cy / H);
    overlay_event(&e, W, H, 1200.0);
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("lift released B", (pad.buttons & WC_BUTTON_B) == 0);

    // dpad glide: press right, slide to up-right, no lift
    SDL_Event d1 = finger(SDL_FINGERDOWN, 8,
        (ctls[dpad_ctl].cx + ctls[dpad_ctl].hx * 0.7f) / W, ctls[dpad_ctl].cy / H);
    overlay_event(&d1, W, H, 1300.0);
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("dpad right", (pad.buttons & WC_BUTTON_RIGHT) != 0);
    SDL_Event d2 = finger(SDL_FINGERMOTION, 8,
        (ctls[dpad_ctl].cx + ctls[dpad_ctl].hx * 0.5f) / W,
        (ctls[dpad_ctl].cy - ctls[dpad_ctl].hx * 0.5f) / H);
    overlay_event(&d2, W, H, 1400.0);
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("dpad glide to up-right",
           (pad.buttons & (WC_BUTTON_UP | WC_BUTTON_RIGHT)) ==
           (WC_BUTTON_UP | WC_BUTTON_RIGHT));

    // hidden overlay: consumes but contributes nothing (must-fail control)
    g_visible = false;
    memset(touches, 0, sizeof(touches));
    dpad_bits = 0;
    e = finger(SDL_FINGERDOWN, 9, ctls[a_idx].cx / W, ctls[a_idx].cy / H);
    expect("hidden still consumes (non-pointer cart)", overlay_event(&e, W, H, 1500.0));
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("hidden contributes nothing", pad.buttons == 0 && pad.connected == 0);

    // stick: floating origin + saturation + dpad emit when both declared
    g_mask = WC_CTRL_DPAD | WC_CTRL_LSTICK | WC_CTRL_A;
    overlay_layout(W, H, game_x, 0, game_w, (float)H);
    g_visible = true;
    expect("stick dial when declared", ctls[dpad_ctl].kind == K_STICK);
    float scx = ctls[dpad_ctl].cx / W, scy = ctls[dpad_ctl].cy / H;
    e = finger(SDL_FINGERDOWN, 3, scx, scy);
    overlay_event(&e, W, H, 1600.0);
    e = finger(SDL_FINGERMOTION, 3, scx + (mm(STICK_THROW_MM) * 2.0f) / W, scy);
    overlay_event(&e, W, H, 1700.0);
    memset(&pad, 0, sizeof(pad));
    overlay_apply(&pad);
    expect("stick saturates right", pad.left_x > 32000);
    expect("stick emits dpad bits when both declared",
           (pad.buttons & WC_BUTTON_RIGHT) != 0);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: overlay logic (eightway, layout, events, stick)\n");
    return 0;
}
