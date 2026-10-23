package com.stratum.runtime;

/**
 * OPTIONAL UTILITY — rare-use-case Java-to-Python dynamic lookup.
 *
 * Nothing in the core pipeline (StratumActivity, StratumView,
 * StratumReceiver, StratumService) depends on this class or is changed
 * by it. It exists purely for custom Java code you write yourself in
 * Android Studio (a third-party SDK callback, a manual click listener,
 * etc.) that needs to call an exported Python function.
 *
 * Example:
 *     Object result = StratumRuntimeLookup.callPython("processPayload", data);
 */
public final class StratumRuntimeLookup {

    private StratumRuntimeLookup() {} // Prevent instantiation

    /**
     * Dispatches a call to an exported function in your Stratum application.
     *
     * @param functionName Name of the function registered in Stratum.
     * @param args         Arguments passed to the function.
     * @return Result returned from the Stratum application, or null.
     */
    public static Object callStratum(String functionName, Object... args) {
        return StratumInvocationHandler.nativeDispatch(
                "app",
                functionName,
                args != null ? args : new Object[0]
        );
    }

    /** Shorthand alias for callStratum */
    public static Object call(String functionName, Object... args) {
        return callStratum(functionName, args);
    }

    /** Backward-compatible alias for existing code */
    public static Object callPython(String functionName, Object... args) {
        return callStratum(functionName, args);
    }
}