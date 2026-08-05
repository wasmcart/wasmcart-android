package dev.wasmcart.player;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Fullscreen player for a single .wasc cart.
 *
 * Cart resolution order:
 *   1. ACTION_VIEW intent (content:// or file:// URI) — the stream is copied
 *      into cacheDir because the native host needs a real file path for the
 *      zip reader (cart.wasm is mmap'd at its stored offset).
 *   2. Bundled asset "cart.wasc" — copied into filesDir on first launch or
 *      when the bundled size changes.
 *
 * The resolved path is handed to native as argv[1]; argv[2] is the saves
 * directory (SDL's getArguments() contract).
 */
public class WasmcartActivity extends SDLActivity {
    private static final String TAG = "wasmcart";
    private String cartPath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        cartPath = resolveCart();
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File saves = new File(getFilesDir(), "saves");
        //noinspection ResultOfMethodCallIgnored
        saves.mkdirs();
        if (cartPath == null) {
            Log.e(TAG, "no cart available (no intent, no bundled asset)");
            return new String[0];
        }
        return new String[] { cartPath, saves.getAbsolutePath() };
    }

    private String resolveCart() {
        Intent intent = getIntent();
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri uri = intent.getData();
            if (uri != null) {
                String p = copyUri(uri, new File(getCacheDir(), "opened.wasc"));
                if (p != null) return p;
                Log.e(TAG, "failed to read " + uri + ", falling back to bundled cart");
            }
        }
        return copyBundledCart();
    }

    private String copyUri(Uri uri, File dest) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) return null;
            copyStream(in, dest);
            return dest.getAbsolutePath();
        } catch (Exception e) {
            Log.e(TAG, "copyUri: " + e);
            return null;
        }
    }

    private String copyBundledCart() {
        File dest = new File(getFilesDir(), "cart.wasc");
        try {
            long assetSize;
            try (InputStream probe = getAssets().open("cart.wasc")) {
                assetSize = probe.available();
            }
            if (!dest.exists() || dest.length() != assetSize) {
                try (InputStream in = getAssets().open("cart.wasc")) {
                    copyStream(in, dest);
                }
                Log.i(TAG, "bundled cart copied (" + dest.length() + " bytes)");
            }
            return dest.getAbsolutePath();
        } catch (Exception e) {
            Log.e(TAG, "no bundled cart: " + e);
            return null;
        }
    }

    private static void copyStream(InputStream in, File dest) throws Exception {
        File tmp = new File(dest.getAbsolutePath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        if (!tmp.renameTo(dest)) throw new RuntimeException("rename failed: " + dest);
    }
}
