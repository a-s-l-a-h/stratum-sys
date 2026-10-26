package com.stratum.runtime;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;

public class StratumInvocationHandler implements InvocationHandler {
    private final String callbackKey;
    public StratumInvocationHandler(String callbackKey) { this.callbackKey = callbackKey; }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String name = method.getName();
        if (name.equals("toString")) return "StratumProxy";
        if (name.equals("hashCode")) return System.identityHashCode(proxy);
        if (name.equals("equals")) return proxy == args[0];
        Object result = nativeDispatch(callbackKey, name, args != null ? args : new Object[0]);
        Class<?> rt = method.getReturnType();
        if (result instanceof Number && rt.isPrimitive()) {
            Number n = (Number) result;
            if (rt == long.class)   return n.longValue();
            if (rt == int.class)    return n.intValue();
            if (rt == float.class)  return n.floatValue();
            if (rt == double.class) return n.doubleValue();
            if (rt == short.class)  return n.shortValue();
            if (rt == byte.class)   return n.byteValue();
        }
        if (result == null && rt.isPrimitive()) {
            if (rt == boolean.class) return Boolean.FALSE;
            if (rt == int.class)     return Integer.valueOf(0);
            if (rt == long.class)    return Long.valueOf(0L);
            if (rt == float.class)   return Float.valueOf(0f);
            if (rt == double.class)  return Double.valueOf(0.0);
            if (rt == byte.class)    return Byte.valueOf((byte) 0);
            if (rt == short.class)   return Short.valueOf((short) 0);
            if (rt == char.class)    return Character.valueOf((char) 0);
        }
        return result;
    }

    // Native closure lives as long as this Java listener. Python engine: no-op.
    @Override
    protected void finalize() throws Throwable {
        try { nativeRelease(callbackKey); }
        catch (UnsatisfiedLinkError ignored) { }
        finally { super.finalize(); }
    }

    public static native Object nativeDispatch(String key, String method, Object[] args);
    public static native void nativeRelease(String key);
}