package com.stratum.runtime;

import android.content.Context;
import android.util.Log;

/**
 * Direct CPython equivalent of Chaquopy's StratumBootstrap.
 *
 * Ensures the runtime state is verified when background components
 * (StratumService, StratumReceiver) are invoked.
 *
 * All .so loading, asset unpacking, and Python initialization are
 * managed by the main activity setup.
 */
public final class StratumBootstrap {
    private static final String TAG = "StratumBootstrap";
    private static volatile boolean sReady = false;

    private StratumBootstrap() {}

    /**
     * Marks the runtime as initialized. Called by StratumActivity once
     * MainActivity has loaded the library and set up the environment.
     */
    public static void markReady() {
        sReady = true;
        Log.i(TAG, "Stratum runtime marked ready by Activity.");
    }

    public static boolean isReady() {
        return sReady;
    }

    /**
     * Ensures the runtime is ready for background operations.
     * If MainActivity already ran, this is an instant no-op.
     * If started in the background (Receiver/Service), this loads libstratum.so as a fallback.
     */
    public static synchronized void ensureReady(Context context) {
        if (sReady) {
            return; // Already loaded and initialized by MainActivity
        }

        try {
            System.loadLibrary("stratum");
            sReady = true;
            Log.i(TAG, "libstratum.so loaded via fallback by StratumBootstrap for background component.");
        } catch (Throwable t) {
            Log.e(TAG, "StratumBootstrap fallback failed to load libstratum.so", t);
        }
    }
}