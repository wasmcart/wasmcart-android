# wasmcart-android

Turn a [wasmcart](https://github.com/wasmcart/wasmcart) cart into a native
Android app. One `.wasc` runs fullscreen with touch and gamepads. No WebView:
the cart runs on V8 (via libnode) behind the same shared C host that powers
the desktop player and the libretro core; this app is the third embedding of
[wasmcart-native](https://github.com/wasmcart/wasmcart-native).

Two ways to use it:

1. **Player**: build it as-is and open any `.wasc` from a file manager, or
   bundle a cart for it to boot into.
2. **App template for your game**: your cart plus this repo is a complete,
   store-ready Android app. See [Ship your cart as an app](#ship-your-cart-as-an-app).

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
`ANDROID_HOME`).

## Ship your cart as an app

The player doubles as a template: bundle your cart and rebrand, and the
result is a self-contained native Android app of YOUR game, no wasmcart
branding, nothing downloaded at runtime.

1. `cp your-game.wasc app/src/main/assets/cart.wasc` - the app boots straight
   into it.
2. In `app/build.gradle`: change `applicationId` (this is your app's identity
   on the store and on devices, so pick it before shipping and never change
   it).
3. In `app/src/main/res/values/strings.xml`: change `app_name` to your title.
4. Add your launcher icon (`android:icon` in the manifest + mipmap resources).
5. `./gradlew assembleRelease` (or `bundleRelease` for Play), sign with your
   own keystore.

Because the game is wasm executed by a bundled VM, this sits inside Play's
interpreted-code carve-out; the cart ships in the APK itself, so nothing
executable is fetched at runtime. Same cart still runs unchanged in the
browser, the desktop player, and RetroArch - the APK is just one more target
built from the same `.wasc`.

## Why is the APK ~60MB?

Because it ships its own WebAssembly engine, and almost nothing else. Android
still provides no system wasm runtime for apps: the excellent V8 sitting in
every device's WebView is not exposed to native code (the closest thing,
`androidx.javascriptengine`, is an out-of-process JS-eval service that can't
drive a frame loop). So every app that needs real wasm performance bundles an
engine - Prime Video ships WAMR, WeChat ships its own runtime for mini-games,
and we ship V8 via libnode.

V8 specifically, because carts are arbitrary third-party code: they use SIMD,
threads, and Emscripten's legacy exception opcodes (every Lua/QuickJS/mruby/
engine-port cart), and they need JIT-class cold starts (a 52MB Godot cart
loads in ~350ms on V8 versus ~29s AOT-compiling on wasmtime). Smaller
runtimes fail one or more of those.

The engine is ~58MB of the APK; everything wasmcart adds is about 1MB. If
Android ever exposes its system V8 to apps, this APK drops to a few MB with
no cart-visible change.

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
