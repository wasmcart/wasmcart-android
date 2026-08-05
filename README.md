# wasmcart-android

Slim standalone Android player for [wasmcart](https://github.com/wasmcart/wasmcart)
carts: one `.wasc` fullscreen, with touch and gamepads. No WebView — the cart
runs on V8 (via libnode) behind the same shared C host that powers the desktop
player and the libretro core; this app is the third embedding of
[wasmcart-native](https://github.com/wasmcart/wasmcart-native).

- **Engine**: V8 (libnode, android-aarch64). Accepts both legacy and
  standardized wasm exception handling, which the scripting-runtime carts
  (Lua, QuickJS, MicroPython, mruby) require, and cold-starts 50MB carts in
  well under a second.
- **Shell**: SDL2's official Android port. `SDLActivity` provides the EGL
  surface, AAudio, touch and gamepad plumbing; `native/android_main.c` is the
  frame loop, transliterated from the desktop player.
- **Touch**: first `wc_host_set_pointer` implementation in any shipping host.
  Fingers map to pointer slots 1-9 (slot 0 is reserved for mice per the ABI),
  with coordinates run through the inverse letterbox transform into cart
  space.
- **Min SDK 33** (Android 13) — that's libnode's floor, not a choice.
  arm64-v8a only.

## Build

```sh
./deps/fetch-deps.sh          # SDL2 source + prebuilt libnode (one-time)
git submodule update --init   # wasmcart-native (shared host)
cp /path/to/some.wasc app/src/main/assets/cart.wasc   # cart to bundle
./gradlew assembleDebug
```

Requires an Android SDK with NDK r27 and CMake 3.22 (`local.properties` or
`ANDROID_HOME`). The APK is dominated by V8 (~80MB of native lib); everything
wasmcart adds is about 1MB.

## Running carts

1. **Bundled**: whatever `app/src/main/assets/cart.wasc` was at build time.
2. **Opened**: the app registers for `.wasc` VIEW intents — open a cart from
   a file manager or Downloads and it plays fullscreen.

Saves live in the app's private `files/saves/`, keyed by cart filename + size,
and are flushed when the app is backgrounded (Android can kill a backgrounded
app without warning), on exit, and on a periodic dirty check.

## Architecture notes

- The GL context is **owned by SDL**. Native code never creates an EGL
  context; every cart GL entry point resolves through
  `wc_host_set_gl_loader(SDL_GL_GetProcAddress)`. GL carts render into the
  shared host's redirect FBO and are letterboxed to the screen by
  `wc_gl_blit_to_screen`.
- 2D carts upload their BGRA framebuffer as an RGBA texture and swizzle in
  the blit shader — no `GL_BGRA_EXT` dependency, no CPU conversion.
- Frame stepping is fixed-step (60Hz cart clock) with wall-clock accumulation,
  a 5-step catch-up cap, and an audio-queue top-up that keeps ~80ms buffered.
- Cart delivery stays inside Play's interpreter carve-out: wasm is executed by
  a VM, nothing is AOT-compiled to native code.
