package com.stratum.runtime;

import android.os.Bundle;
import android.util.Log;
import androidx.appcompat.app.AppCompatActivity;

/**
 * Base activity for Stratum applications using direct embedded CPython.
 * Handles JVM library loading, Activity handoff to C++, and lifecycle forwarding.
 */
public class StratumActivity extends AppCompatActivity {

    private static final String TAG = "StratumActivity";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // ── Step 1: Ensure libstratum.so is loaded in JVM ──────────────────
        try {
            System.loadLibrary("stratum");
            Log.i(TAG, "libstratum.so loaded into JVM.");
        } catch (UnsatisfiedLinkError e) {
            String msg = e.getMessage() != null ? e.getMessage() : "";
            if (!msg.contains("already loaded")) {
                throw new RuntimeException("[Stratum] Failed to load libstratum.so: " + msg, e);
            }
        }

        // ── Step 2: Hand the Android Activity reference to C++ bridge ──────
        // Must happen BEFORE nativeOnCreate() so getActivity() works in native code
        nativeSetActivity(this);
        StratumBootstrap.markReady();
        Log.i(TAG, "Activity reference registered and StratumBootstrap marked ready.");

        // ── Step 3: Import main.py ─────────────────────────────────────────
        if (!onImportMain()) {
            throw new RuntimeException("[Stratum] onImportMain() returned false.");
        }
        Log.i(TAG, "Native entry loaded.");

        // ── Step 4: Fire onCreate lifecycle event into Python ──────────────
        nativeOnCreate();
        Log.i(TAG, "nativeOnCreate dispatched.");
    }

    /**
     * Subclasses (MainActivity) override this to route module import through PythonHost.
     */
    protected boolean onImportMain() {
        return true;
    }

    /**
     * Checks if the Stratum application consumes the hardware back-press.
     * Returns true if handled, preventing default Android finish/exit action.
     */
    protected boolean onBackPressedStratum() {
        Object r = StratumRuntimeLookup.callStratum("onBackPressed");
        return (r instanceof Boolean) && (Boolean) r;
    }

    // ── Hardware Back Button Intercept ─────────────────────────────────────
    @Override
    public void onBackPressed() {
        try {
            if (onBackPressedStratum()) {
                return; // Stratum application consumed the back action
            }
        } catch (Throwable t) {
            Log.e(TAG, "Error routing onBackPressed to Stratum", t);
        }
        // Not consumed; let Android handle default back behavior
        super.onBackPressed();
    }

    // ── Lifecycle forwarding to C++ bridge ─────────────────────────────────
    @Override
    protected void onResume() {
        super.onResume();
        nativeOnResume();
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativeOnPause();
    }

    @Override
    protected void onStop() {
        super.onStop();
        nativeOnStop();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeOnDestroy();
    }

    // ── Native JNI methods exported by libstratum.so ───────────────────────
    private static native void nativeSetActivity(Object activity);
    private static native void nativeOnCreate();
    private static native void nativeOnResume();
    private static native void nativeOnPause();
    private static native void nativeOnStop();
    private static native void nativeOnDestroy();
}