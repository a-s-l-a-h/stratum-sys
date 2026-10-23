// stratum.reflect equivalent: call ANY Java class (your own app classes, third-party SDKs) with no generated wrapper.
#include "bridge_core.h"
#include <cstring>
#include <vector>

static thread_local std::string t_rstr;

extern "C" int32_t stratum_reflect_call(const char* class_name, int64_t target, const char* method,
                                        const StratumVal* args, size_t argc, StratumVal* out) {
    int32_t err = 0;
    guarded<int>(&err, 0, [&]() -> int {
        JNIEnv* env = get_env(); if (!env) throw std::runtime_error("Stratum: no JNIEnv");
        JniLocalFrame fr(env, 256);
        memset(out, 0, sizeof(*out)); out->tag = STRATUM_V_NULL;

        jclass cls = nullptr;
        if (class_name && *class_name) { std::string n = class_name; for (char& c : n) if (c == '.') c = '/'; cls = find_class(env, n.c_str()); }
        else if (target) cls = env->GetObjectClass((jobject)(uintptr_t)target);
        if (!cls) throw std::runtime_error(std::string("reflect: class not found: ") + (class_name ? class_name : "?"));

        jclass cMethod = find_class(env, "java/lang/reflect/Method"), cCtor = find_class(env, "java/lang/reflect/Constructor");
        jmethodID getMethods = env->GetMethodID(g_class_class, "getMethods", "()[Ljava/lang/reflect/Method;");
        jmethodID getCtors = env->GetMethodID(g_class_class, "getConstructors", "()[Ljava/lang/reflect/Constructor;");
        jmethodID isPrim = env->GetMethodID(g_class_class, "isPrimitive", "()Z");
        jmethodID isInst = env->GetMethodID(g_class_class, "isInstance", "(Ljava/lang/Object;)Z");
        jmethodID clsName = env->GetMethodID(g_class_class, "getName", "()Ljava/lang/String;");
        jmethodID mName = env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;");
        jmethodID mMods = env->GetMethodID(cMethod, "getModifiers", "()I");
        jmethodID mPtypes = env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;");
        jmethodID mInvoke = env->GetMethodID(cMethod, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        jmethodID mAcc = env->GetMethodID(cMethod, "setAccessible", "(Z)V");
        jmethodID cPtypes = env->GetMethodID(cCtor, "getParameterTypes", "()[Ljava/lang/Class;");
        jmethodID cNew = env->GetMethodID(cCtor, "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;");
        jmethodID cAcc = env->GetMethodID(cCtor, "setAccessible", "(Z)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); throw std::runtime_error("reflect: reflection API unavailable"); }

        // box arguments
        jobjectArray jargs = env->NewObjectArray((jsize)argc, g_object_class, nullptr);
        std::vector<jobject> boxed(argc, nullptr);
        for (size_t i = 0; i < argc; ++i) { boxed[i] = box_value(env, args[i]); env->SetObjectArrayElement(jargs, (jsize)i, boxed[i]); }

        auto score = [&](jobject pt, jobject a) -> int {          // 0 = no match, 1 = widening, 2 = exact
            bool prim = env->CallBooleanMethod(pt, isPrim);
            if (!a) return prim ? 0 : 2;
            if (!prim) return env->CallBooleanMethod(pt, isInst, a) ? 2 : 0;
            std::string n = stratum_jstring_to_str(env, (jstring)env->CallObjectMethod(pt, clsName));
            auto is = [&](int k) { return env->IsInstanceOf(a, g_prims[k].c) != JNI_FALSE; };   // Z0 B1 C2 S3 I4 J5 F6 D7
            if (n == "boolean") return is(0) ? 2 : 0;
            if (n == "byte")    return is(1) ? 2 : 0;
            if (n == "char")    return is(2) ? 2 : 0;
            if (n == "short")   return is(3) ? 2 : (is(1) ? 1 : 0);
            if (n == "int")     return is(4) ? 2 : ((is(1) || is(3)) ? 1 : 0);
            if (n == "long")    return is(5) ? 2 : ((is(1) || is(3) || is(4)) ? 1 : 0);
            if (n == "float")   return is(6) ? 2 : ((is(4) || is(5)) ? 1 : 0);
            if (n == "double")  return is(7) ? 2 : ((is(6) || is(4) || is(5)) ? 1 : 0);
            return 0;
        };
        auto total = [&](jobjectArray ptypes) -> int {
            jsize n = env->GetArrayLength(ptypes);
            if ((size_t)n != argc) return -1;
            int sum = 0;
            for (jsize k = 0; k < n; ++k) {
                jobject pt = env->GetObjectArrayElement(ptypes, k);
                int s = score(pt, boxed[k]); env->DeleteLocalRef(pt);
                if (s == 0) return -1; sum += s;
            }
            return sum;
        };

        jobject result = nullptr;
        if (!method) {                                              // constructor
            jobjectArray ctors = (jobjectArray)env->CallObjectMethod(cls, getCtors);
            jobject best = nullptr; int bestScore = -1;
            for (jsize i = 0, n = env->GetArrayLength(ctors); i < n; ++i) {
                jobject c = env->GetObjectArrayElement(ctors, i);
                jobjectArray pts = (jobjectArray)env->CallObjectMethod(c, cPtypes);
                int s = total(pts);
                if (s > bestScore) { bestScore = s; best = c; }
                env->DeleteLocalRef(pts);
            }
            if (!best) throw std::runtime_error("reflect: no matching constructor");
            env->CallVoidMethod(best, cAcc, JNI_TRUE);
            result = env->CallObjectMethod(best, cNew, jargs);
        } else {
            jobjectArray ms = (jobjectArray)env->CallObjectMethod(cls, getMethods);
            jobject best = nullptr; int bestScore = -1;
            for (jsize i = 0, n = env->GetArrayLength(ms); i < n; ++i) {
                jobject m = env->GetObjectArrayElement(ms, i);
                std::string nm = stratum_jstring_to_str(env, (jstring)env->CallObjectMethod(m, mName));
                bool is_static = (env->CallIntMethod(m, mMods) & 0x8) != 0;
                if (nm != method || is_static != (target == 0)) continue;
                jobjectArray pts = (jobjectArray)env->CallObjectMethod(m, mPtypes);
                int s = total(pts);
                if (s > bestScore) { bestScore = s; best = m; }
                env->DeleteLocalRef(pts);
            }
            if (!best) throw std::runtime_error(std::string("reflect: no matching ") + (target ? "method " : "static method ") + method);
            env->CallVoidMethod(best, mAcc, JNI_TRUE);
            result = env->CallObjectMethod(best, mInvoke, (jobject)(uintptr_t)target, jargs);   // target 0 -> null (static)
        }
        stratum_check_java_exc(env);                                // unwraps InvocationTargetException
        java_to_val(env, result, *out, t_rstr, true);               // OBJ -> new global ref owned by caller
        return 0;
    });
    return err;
}