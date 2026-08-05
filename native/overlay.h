// overlay.h — optional on-screen gamepad for carts that don't use the pointer.
//
// Shown only when no physical controller is connected AND the cart did not
// set WC_FLAG_POINTER. Which controls appear comes from the manifest
// `controls` presentation hint (WC_CTRL_* mask; WC_CTRL_DEFAULT_SET when the
// manifest omits it). Design + prior-art rationale:
// ~/code/cliemu/wasmcart-android-overlay-plan.md (not shipped in this repo).

#ifndef WASMCART_OVERLAY_H
#define WASMCART_OVERLAY_H

#include "wasmcart_host.h"
#include "SDL.h"
#include <stdbool.h>

// Create GL resources + haptics. Call once with the GL context current.
// px_per_mm: physical scale (from display DPI); controls sized in mm, not px.
void overlay_init(uint32_t controls_mask, float px_per_mm);

// Recompute positions. game_rect_* is the letterboxed cart rect in window
// pixels — dials center in the bezel bars when the bars are wide enough.
void overlay_layout(int win_w, int win_h,
                    float game_x, float game_y, float game_w, float game_h);

// Fade in/out (controller hotplug). Input stops the moment hiding starts.
void overlay_set_visible(bool visible);

// Feed SDL events. Returns true if the event was consumed by the overlay.
// Only finger events are ever consumed.
bool overlay_event(const SDL_Event* ev, int win_w, int win_h, double now_ms);

// OR the overlay's current state into a pad (call after poll_pads, before
// wc_host_set_pads). Marks the pad connected while the overlay is visible.
void overlay_apply(wc_pad_t* pad);

// Draw. Call after the cart's frame is on FBO 0, before SDL_GL_SwapWindow.
// save_gl_state: true for GL carts (their state caches must survive us).
void overlay_render(bool save_gl_state, double now_ms);

void overlay_shutdown(void);

#endif // WASMCART_OVERLAY_H
