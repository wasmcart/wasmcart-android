// android_main.c — wasmcart Android player (SDL2 shell over the shared host)
//
// Third embedding of libwasmcart: wasmcart-native/src/main.c is the desktop
// player, wasmcart-libretro/src/libretro.c is the RetroArch core, this is the
// standalone Android app. Most of the frame loop is transliterated from the
// desktop main.c; what's new here is touch (the first wc_host_set_pointer
// implementation in any host), Android lifecycle handling, and audio-paced
// catch-up stepping.
//
// GL context is OWNED BY SDL (never create EGL here); every cart GL entry
// point flows through wc_host_set_gl_loader(SDL_GL_GetProcAddress). The GLES3
// calls in this file are only the app's own 2D blit path.
//
// argv[1] = cart path, argv[2] = saves directory (from WasmcartActivity).

#include "wasmcart_host.h"
#include "wc_log.h"
#include "overlay.h"

#include "SDL.h"
#include "SDL_main.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTROLLERS 4
#define CART_FPS 60.0
#define STEP_MS (1000.0 / CART_FPS)
#define MAX_STEPS 5          // catch-up cap per displayed frame
#define AUDIO_TARGET_MS 80   // keep this much queued (romdev playtest pacing)
#define SAVE_CHECK_FRAMES 600

// Shared-host internals also used by the desktop player (gl_imports.cpp)
extern void wc_gl_setup_redirect(uint32_t width, uint32_t height);
extern void wc_gl_blit_to_screen(uint32_t cart_w, uint32_t cart_h, uint32_t win_w, uint32_t win_h);

static SDL_GameController* controllers[MAX_CONTROLLERS] = {0};

// ─── Controllers (same shape as desktop main.c) ────────────────────────────

static void open_controller(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (!controllers[i]) {
            controllers[i] = SDL_GameControllerOpen(device_index);
            if (controllers[i]) {
                wc_log("controller %d connected: %s\n", i, SDL_GameControllerName(controllers[i]));
            }
            return;
        }
    }
}

static bool any_controller(void) {
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i]) return true;
    return false;
}

static void close_controller(SDL_JoystickID id) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (controllers[i] &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[i])) == id) {
            wc_log("controller %d disconnected\n", i);
            SDL_GameControllerClose(controllers[i]);
            controllers[i] = NULL;
            return;
        }
    }
}

static void poll_pads(wc_pad_t pads[WC_MAX_PADS]) {
    memset(pads, 0, sizeof(wc_pad_t) * WC_MAX_PADS);
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        SDL_GameController* gc = controllers[i];
        if (!gc) continue;
        pads[i].connected = 1;

        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))            pads[i].buttons |= WC_BUTTON_A;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))            pads[i].buttons |= WC_BUTTON_B;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))            pads[i].buttons |= WC_BUTTON_X;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))            pads[i].buttons |= WC_BUTTON_Y;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pads[i].buttons |= WC_BUTTON_L;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))pads[i].buttons |= WC_BUTTON_R;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))        pads[i].buttons |= WC_BUTTON_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))         pads[i].buttons |= WC_BUTTON_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))      pads[i].buttons |= WC_BUTTON_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))    pads[i].buttons |= WC_BUTTON_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))    pads[i].buttons |= WC_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))   pads[i].buttons |= WC_BUTTON_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))    pads[i].buttons |= WC_BUTTON_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))   pads[i].buttons |= WC_BUTTON_R3;

        pads[i].left_x  = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        pads[i].left_y  = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        pads[i].right_x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        pads[i].right_y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        pads[i].left_trigger  = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        pads[i].right_trigger = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    }
}

// ─── Rumble backend → SDL_GameControllerRumble (InputDevice vibrator) ──────

static int rumble_has(void* user, uint32_t pad_id) {
    (void)user;
    return pad_id < MAX_CONTROLLERS && controllers[pad_id] &&
           SDL_GameControllerHasRumble(controllers[pad_id]);
}
static void rumble_go(void* user, uint32_t pad_id, float low, float high, uint32_t ms) {
    (void)user;
    if (pad_id < MAX_CONTROLLERS && controllers[pad_id])
        SDL_GameControllerRumble(controllers[pad_id],
            (Uint16)(low * 65535.0f), (Uint16)(high * 65535.0f), ms);
}
static void rumble_stop(void* user, uint32_t pad_id) {
    (void)user;
    if (pad_id < MAX_CONTROLLERS && controllers[pad_id])
        SDL_GameControllerRumble(controllers[pad_id], 0, 0, 0);
}

// ─── Letterbox (mirror of letterbox.js fitRect: min scale, centered) ───────

typedef struct { float x, y, w, h; } fit_rect_t;

static fit_rect_t fit_rect(uint32_t src_w, uint32_t src_h, int dst_w, int dst_h) {
    fit_rect_t r = {0, 0, (float)dst_w, (float)dst_h};
    if (!src_w || !src_h) return r;
    float scale = fminf((float)dst_w / (float)src_w, (float)dst_h / (float)src_h);
    r.w = (float)src_w * scale;
    r.h = (float)src_h * scale;
    r.x = ((float)dst_w - r.w) * 0.5f;
    r.y = ((float)dst_h - r.h) * 0.5f;
    return r;
}

// ─── Pointer: touch fingers → slots 1-9, mouse → slot 0 ────────────────────
//
// First wc_host_set_pointer implementation in any shipping host. Slot 0 is
// reserved for mouse by the ABI (SPEC.md pointer section); fingers fill 1+.
// SDL finger IDs are arbitrary 64-bit values and MUST NOT pass through raw —
// we keep our own id→slot table. SDL_HINT_TOUCH_MOUSE_EVENTS=0 stops SDL from
// synthesizing ghost mouse events into slot 0 from touches.

typedef struct {
    SDL_FingerID id;
    bool used;
} finger_slot_t;

static finger_slot_t finger_slots[WC_MAX_POINTERS]; // [0] unused (mouse)

static int finger_slot_find(SDL_FingerID id) {
    for (int i = 1; i < WC_MAX_POINTERS; i++)
        if (finger_slots[i].used && finger_slots[i].id == id) return i;
    return -1;
}

static int finger_slot_alloc(SDL_FingerID id) {
    int existing = finger_slot_find(id);
    if (existing >= 0) return existing;
    for (int i = 1; i < WC_MAX_POINTERS; i++) {
        if (!finger_slots[i].used) {
            finger_slots[i].used = true;
            finger_slots[i].id = id;
            return i;
        }
    }
    return -1; // >9 concurrent fingers: drop
}

static void finger_slot_free(int slot) {
    if (slot >= 1 && slot < WC_MAX_POINTERS) finger_slots[slot].used = false;
}

// Window coords → cart coords through the inverse letterbox transform.
// Touches in the letterbox bars clamp into range.
static void to_cart_coords(float px, float py, const fit_rect_t* r,
                           uint32_t cart_w, uint32_t cart_h,
                           int16_t* cx, int16_t* cy) {
    float x = (px - r->x) * (float)cart_w / r->w;
    float y = (py - r->y) * (float)cart_h / r->h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (cart_w && x > (float)(cart_w - 1)) x = (float)(cart_w - 1);
    if (cart_h && y > (float)(cart_h - 1)) y = (float)(cart_h - 1);
    *cx = (int16_t)x;
    *cy = (int16_t)y;
}

// ─── Persistent save ────────────────────────────────────────────────────────
//
// Keyed by cart basename + file size (the manifest name is only known after
// load, but save_data must be in memory BEFORE wc_init reads it). Android
// kills backgrounded apps without warning, so the write points are:
// WILLENTERBACKGROUND, TERMINATING, clean quit, and a dirty-check every
// SAVE_CHECK_FRAMES frames.

static char save_path[1024];
static uint8_t* save_last = NULL;  // last bytes written to disk
static uint32_t save_last_size = 0;

static void save_path_init(const char* saves_dir, const char* cart_path) {
    const char* base = strrchr(cart_path, '/');
    base = base ? base + 1 : cart_path;
    char name[256];
    snprintf(name, sizeof(name), "%s", base);
    char* dot = strrchr(name, '.');
    if (dot) *dot = 0;

    long size = 0;
    FILE* f = fopen(cart_path, "rb");
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }

    snprintf(save_path, sizeof(save_path), "%s/%s-%ld.sav", saves_dir, name, size);
}

static uint8_t* read_file(const char* path, uint32_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    uint8_t* buf = malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); buf = NULL; }
    fclose(f);
    *out_size = buf ? (uint32_t)size : 0;
    return buf;
}

static bool all_zero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

static void save_persist(wc_host_t* host) {
    if (!save_path[0]) return;
    uint32_t size = 0;
    uint8_t* data = wc_host_get_save_data(host, &size);
    if (!data || size == 0) return;

    // Never-written cart with an all-zero region: nothing to say yet
    if (!save_last && all_zero(data, size)) return;
    if (save_last && save_last_size == size && memcmp(save_last, data, size) == 0) return;

    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", save_path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { wc_log_err("save: cannot open %s\n", tmp); return; }
    size_t wrote = fwrite(data, 1, size, f);
    fclose(f);
    if (wrote != size || rename(tmp, save_path) != 0) {
        wc_log_err("save: write failed (%s)\n", save_path);
        return;
    }

    if (!save_last || save_last_size != size) {
        free(save_last);
        save_last = malloc(size);
        save_last_size = size;
    }
    if (save_last) memcpy(save_last, data, size);
    wc_log("save: %u bytes -> %s\n", size, save_path);
}

// ─── 2D framebuffer blit ────────────────────────────────────────────────────
//
// The cart framebuffer is XRGB little-endian, i.e. B,G,R,A byte order. Upload
// as RGBA and swizzle in the shader (.bgra) — no GL_BGRA_EXT dependency, no
// per-pixel CPU conversion.

static GLuint blit_tex = 0, blit_program = 0, blit_vao = 0;
static uint32_t blit_tex_w = 0, blit_tex_h = 0;

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        wc_log_err("blit shader: %s\n", log);
    }
    return s;
}

static void blit_init(void) {
    const char* vs_src =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "  float x = float((gl_VertexID & 1) << 2) - 1.0;\n"
        "  float y = float((gl_VertexID & 2) << 1) - 1.0;\n"
        "  uv = vec2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);\n"
        "  gl_Position = vec4(x, y, 0.0, 1.0);\n"
        "}\n";
    const char* fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 uv;\n"
        "out vec4 fragColor;\n"
        "uniform sampler2D tex;\n"
        "void main() { fragColor = vec4(texture(tex, uv).bgr, 1.0); }\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    blit_program = glCreateProgram();
    glAttachShader(blit_program, vs);
    glAttachShader(blit_program, fs);
    glLinkProgram(blit_program);
    GLint blit_linked = 0;
    glGetProgramiv(blit_program, GL_LINK_STATUS, &blit_linked);
    if (!blit_linked) {
        char log[512];
        glGetProgramInfoLog(blit_program, sizeof(log), NULL, log);
        wc_log_err("blit program link: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGenVertexArrays(1, &blit_vao);
    glGenTextures(1, &blit_tex);
    glBindTexture(GL_TEXTURE_2D, blit_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void blit_2d_frame(const uint8_t* fb, uint32_t w, uint32_t h,
                          int win_w, int win_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blit_tex);
    if (w != blit_tex_w || h != blit_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fb);
        blit_tex_w = w;
        blit_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    }

    fit_rect_t r = fit_rect(w, h, win_w, win_h);
    // GL viewport origin is bottom-left; fit_rect is top-left based, but the
    // rect is vertically centered so the flip is a no-op for y here.
    glViewport((GLint)r.x, (GLint)r.y, (GLsizei)r.w, (GLsizei)r.h);
    glUseProgram(blit_program);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        wc_log_err("no cart path in arguments (WasmcartActivity found no cart)\n");
        return 1;
    }
    const char* cart_path = argv[1];
    const char* saves_dir = argc > 2 ? argv[2] : NULL;

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // no ghost mouse from touch
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0"); // no ghost touch from mouse
    // Without this SDL calls setRequestedOrientation(FULL_USER) on the activity,
    // silently overriding the manifest's sensorLandscape and letting a portrait
    // rotation squash the cart. Both landscape faces, so the device can still be
    // held either way round.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        wc_log_err("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Extra mappings on top of SDL's Android defaults
    {
        #include "gamecontrollerdb.h"
        int count = 0;
        for (const char** p = _gamecontrollerdb_lines; *p; p++) {
            if (SDL_GameControllerAddMapping(*p) >= 0) count++;
        }
        wc_log("loaded %d controller mappings\n", count);
    }

    // GLES 3.0 context, owned by SDL. The window surface needs no depth —
    // GL carts render into the redirect FBO which carries its own depth24/s8.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window = SDL_CreateWindow("wasmcart",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        wc_log_err("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        wc_log_err("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    wc_log("display %dx%d, GL: %s\n", win_w, win_h, (const char*)glGetString(GL_VERSION));

    // Host
    wc_host_t* host = wc_host_create();
    if (!host) {
        wc_log_err("wc_host_create failed\n");
        return 1;
    }
    wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)SDL_GL_GetProcAddress);

    wc_rumble_backend_t rumble = { rumble_has, rumble_go, rumble_stop, NULL };
    wc_host_set_rumble_backend(host, &rumble);

    // Prior save must be in memory before wc_init reads it
    uint8_t* boot_save = NULL;
    uint32_t boot_save_size = 0;
    if (saves_dir) {
        save_path_init(saves_dir, cart_path);
        boot_save = read_file(save_path, &boot_save_size);
        if (boot_save) {
            wc_log("save: restored %u bytes from %s\n", boot_save_size, save_path);
            save_last = malloc(boot_save_size);
            if (save_last) { memcpy(save_last, boot_save, boot_save_size); save_last_size = boot_save_size; }
        }
    }

    wc_host_options_t opts = {
        .preferred_width = (uint32_t)win_w,
        .preferred_height = (uint32_t)win_h,
        .host_fps = (uint32_t)CART_FPS,
        .audio_sample_rate = 48000,
        .save_data = boot_save,
        .save_data_size = boot_save_size,
    };
    int rc = wc_host_load_file(host, cart_path, &opts);
    free(boot_save);
    if (rc != 0) {
        wc_log_err("failed to load %s\n", cart_path);
        wc_host_destroy(host);
        return 1;
    }

    const wc_cart_info_t* info = wc_host_get_cart_info(host);
    const wc_manifest_t* manifest = wc_host_get_manifest(host);
    bool is_gl = wc_host_uses_gl(host);
    uint32_t cart_w = info->width;
    uint32_t cart_h = info->height;

    // Redirect FBO at the real render resolution (desktop main.c pattern)
    uint32_t redir_w = cart_w > (uint32_t)win_w ? cart_w : (uint32_t)win_w;
    uint32_t redir_h = cart_h > (uint32_t)win_h ? cart_h : (uint32_t)win_h;
    if (is_gl) {
        wc_gl_setup_redirect(redir_w, redir_h);
    } else {
        blit_init();
    }

    // On-screen gamepad: only for carts that don't own the touchscreen.
    // Which controls appear comes from the manifest `controls` hint.
    bool overlay_on = (info->flags & WC_FLAG_POINTER) == 0;
    if (overlay_on) {
        float ddpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) != 0 || ddpi <= 0.0f)
            ddpi = 420.0f; // typical 1080p-class panel
        uint32_t mask = manifest->controls_set ? manifest->controls
                                               : WC_CTRL_DEFAULT_SET;
        overlay_init(mask, ddpi / 25.4f);
        fit_rect_t gr = fit_rect(cart_w, cart_h, win_w, win_h);
        overlay_layout(win_w, win_h, gr.x, gr.y, gr.w, gr.h);
        wc_log("overlay: mask %03x (%s), dpi %.0f\n", mask,
               manifest->controls_set ? "manifest" : "default set", ddpi);
    }

    // Audio
    SDL_AudioDeviceID audio_dev = 0;
    bool audio_f32 = (info->flags & WC_FLAG_AUDIO_F32) != 0;
    uint32_t audio_rate = info->audio_sample_rate ? info->audio_sample_rate : 48000;
    uint32_t audio_bytes_per_frame = audio_f32 ? 8 : 4; // stereo
    if (info->audio_ptr && info->audio_cap) {
        SDL_AudioSpec want = {0}, have;
        want.freq = (int)audio_rate;
        want.format = audio_f32 ? AUDIO_F32 : AUDIO_S16;
        want.channels = 2;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) {
            uint32_t seed_bytes = (uint32_t)have.freq / 20 * audio_bytes_per_frame; // 50ms
            uint8_t* silence = calloc(1, seed_bytes);
            SDL_QueueAudio(audio_dev, silence, seed_bytes);
            free(silence);
            SDL_PauseAudioDevice(audio_dev, 0);
            wc_log("audio %dHz %s stereo\n", have.freq, audio_f32 ? "F32" : "S16");
        }
    }
    uint32_t audio_target_bytes = audio_rate * AUDIO_TARGET_MS / 1000 * audio_bytes_per_frame;

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_controller(i);
    if (overlay_on) overlay_set_visible(!any_controller());

    wc_log("running %s (%ux%u, %s)\n", manifest->name, cart_w, cart_h, is_gl ? "GL" : "2D");

    // ── Main loop ──
    wc_host_enter_v8();

    bool running = true;
    bool text_started = false;
    bool audio_seen = false;
    bool suspended = false;
    uint32_t frame_count = 0;
    double time_ms = 0.0;       // cart clock: advances only when frames run
    double acc_ms = 0.0;
    uint64_t last_ticks = SDL_GetTicks64();
    wc_pad_t pads[WC_MAX_PADS];

    while (running) {
        // Mirror cart text-input state → raises/dismisses the soft keyboard
        {
            bool want = wc_host_text_input_active(host) != 0;
            if (want != text_started) {
                if (want) SDL_StartTextInput(); else SDL_StopTextInput();
                text_started = want;
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    save_persist(host);
                    running = false;
                    break;

                case SDL_APP_WILLENTERBACKGROUND:
                case SDL_APP_TERMINATING:
                    // Android kills backgrounded apps without warning: this is
                    // the last reliable write point (SPEC.md lifecycle notes).
                    save_persist(host);
                    suspended = true;
                    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 1);
                    break;

                case SDL_APP_DIDENTERFOREGROUND:
                    suspended = false;
                    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
                    // Absorb the time spent away instead of fast-forwarding
                    last_ticks = SDL_GetTicks64();
                    acc_ms = 0.0;
                    break;

                case SDL_TEXTINPUT:
                    wc_host_push_text(host, ev.text.text, (uint32_t)strlen(ev.text.text));
                    break;

                case SDL_CONTROLLERDEVICEADDED:
                    open_controller(ev.cdevice.which);
                    if (overlay_on) overlay_set_visible(!any_controller());
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    close_controller(ev.cdevice.which);
                    if (overlay_on) overlay_set_visible(!any_controller());
                    break;

                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_AC_BACK) {
                        save_persist(host);
                        running = false;
                    }
                    break;

                // Touch → overlay (non-pointer carts) or pointer slots 1-9
                case SDL_FINGERDOWN:
                case SDL_FINGERMOTION:
                case SDL_FINGERUP: {
                    if (overlay_on) {
                        overlay_event(&ev, win_w, win_h, (double)SDL_GetTicks64());
                        break;
                    }
                    int slot = (ev.type == SDL_FINGERDOWN)
                        ? finger_slot_alloc(ev.tfinger.fingerId)
                        : finger_slot_find(ev.tfinger.fingerId);
                    if (slot < 0) break;
                    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
                    fit_rect_t r = fit_rect(ci->width, ci->height, win_w, win_h);
                    int16_t cx, cy;
                    to_cart_coords(ev.tfinger.x * (float)win_w,
                                   ev.tfinger.y * (float)win_h,
                                   &r, ci->width, ci->height, &cx, &cy);
                    if (ev.type == SDL_FINGERUP) {
                        wc_host_set_pointer(host, slot, cx, cy, 0, 0);
                        finger_slot_free(slot);
                    } else {
                        wc_host_set_pointer(host, slot, cx, cy, 0x01, 1);
                    }
                    break;
                }

                // Real mice (touch synthesis disabled above) → slot 0
                case SDL_MOUSEMOTION:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    if (ev.type != SDL_MOUSEMOTION && ev.button.which == SDL_TOUCH_MOUSEID) break;
                    int mx, my;
                    uint32_t state = SDL_GetMouseState(&mx, &my);
                    uint8_t buttons = 0;
                    if (state & SDL_BUTTON_LMASK) buttons |= 0x01;
                    if (state & SDL_BUTTON_RMASK) buttons |= 0x02;
                    if (state & SDL_BUTTON_MMASK) buttons |= 0x04;
                    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
                    fit_rect_t r = fit_rect(ci->width, ci->height, win_w, win_h);
                    int16_t cx, cy;
                    to_cart_coords((float)mx, (float)my, &r, ci->width, ci->height, &cx, &cy);
                    wc_host_set_pointer(host, 0, cx, cy, buttons, 1);
                    break;
                }

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        SDL_GL_GetDrawableSize(window, &win_w, &win_h);
                        if (overlay_on) {
                            const wc_cart_info_t* ci = wc_host_get_cart_info(host);
                            fit_rect_t gr = fit_rect(ci->width, ci->height, win_w, win_h);
                            overlay_layout(win_w, win_h, gr.x, gr.y, gr.w, gr.h);
                        }
                    }
                    break;
            }
        }
        if (!running) break;

        uint64_t now = SDL_GetTicks64();
        double real_delta = (double)(now - last_ticks);
        last_ticks = now;
        if (real_delta > 250.0) real_delta = 250.0; // resume/hiccup clamp (SPEC)

        if (suspended) {
            // While suspended: do not call wc_render. SDL usually blocks in
            // PollEvent on pause anyway; this covers the non-blocking window.
            SDL_Delay(50);
            continue;
        }

        acc_ms += real_delta;

        poll_pads(pads);
        if (overlay_on) overlay_apply(&pads[0]);
        wc_host_set_pads(host, pads);

        // Fixed-step with catch-up: wall clock paces, audio queue refines.
        int steps = 0;
        while (acc_ms >= STEP_MS && steps < MAX_STEPS && running) {
            time_ms += STEP_MS;
            wc_host_set_time(host, time_ms, STEP_MS, frame_count);
            wc_host_run_frame(host);
            frame_count++;
            steps++;
            acc_ms -= STEP_MS;

            if (audio_dev) {
                uint32_t n;
                bool f32;
                const void* audio = wc_host_get_audio(host, &n, &f32);
                if (n > 0) {
                    SDL_QueueAudio(audio_dev, audio, n * (f32 ? 8 : 4));
                    audio_seen = true;
                }
            }
            if (wc_host_has_trapped(host)) {
                wc_log_err("cart trapped at frame %u\n", frame_count);
                save_persist(host);
                running = false;
            }
        }
        if (steps == MAX_STEPS) acc_ms = 0.0; // dropped time, don't spiral

        // Audio-paced top-up: keep ~AUDIO_TARGET_MS queued so the device never
        // underruns (the anti-choppiness rule from romdev playtest pacing).
        int extra = 0;
        while (running && audio_dev && audio_seen && extra < 3 &&
               SDL_GetQueuedAudioSize(audio_dev) < audio_target_bytes) {
            time_ms += STEP_MS;
            wc_host_set_time(host, time_ms, STEP_MS, frame_count);
            wc_host_run_frame(host);
            frame_count++;
            extra++;
            uint32_t n;
            bool f32;
            const void* audio = wc_host_get_audio(host, &n, &f32);
            if (n > 0) SDL_QueueAudio(audio_dev, audio, n * (f32 ? 8 : 4));
            else break; // cart went quiet; don't spin
            if (wc_host_has_trapped(host)) {
                wc_log_err("cart trapped at frame %u\n", frame_count);
                save_persist(host);
                running = false;
            }
        }

        // First frame: cart may have resized (Godot reads host_info and
        // reconfigures) — grow the redirect FBO to match.
        if (frame_count > 0 && frame_count <= (uint32_t)MAX_STEPS && is_gl) {
            const wc_cart_info_t* ni = wc_host_get_cart_info(host);
            if (ni->width != cart_w || ni->height != cart_h) {
                cart_w = ni->width;
                cart_h = ni->height;
                redir_w = cart_w > (uint32_t)win_w ? cart_w : (uint32_t)win_w;
                redir_h = cart_h > (uint32_t)win_h ? cart_h : (uint32_t)win_h;
                wc_gl_setup_redirect(redir_w, redir_h);
                wc_log("redirect FBO resized to %ux%u (cart %ux%u)\n",
                       redir_w, redir_h, cart_w, cart_h);
                if (overlay_on) {
                    fit_rect_t gr = fit_rect(cart_w, cart_h, win_w, win_h);
                    overlay_layout(win_w, win_h, gr.x, gr.y, gr.w, gr.h);
                }
            }
        }

        // Present
        if (is_gl) {
            wc_gl_blit_to_screen(redir_w, redir_h, (uint32_t)win_w, (uint32_t)win_h);
        } else {
            uint32_t w, h;
            const uint8_t* fb = wc_host_get_framebuffer(host, &w, &h);
            if (fb && w > 0 && h > 0) blit_2d_frame(fb, w, h, win_w, win_h);
        }
        if (overlay_on) overlay_render(is_gl, (double)now);
        SDL_GL_SwapWindow(window); // vsync paces the loop

        if (frame_count % SAVE_CHECK_FRAMES == 0) save_persist(host);
    }

    save_persist(host);
    wc_host_exit_v8();
    wc_log("shutting down\n");

    if (overlay_on) overlay_shutdown();
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i]) SDL_GameControllerClose(controllers[i]);
    wc_host_destroy(host);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
