// Init, JNI natives (dispatch/release/lifecycle), UI helpers, callback API, logging.
#include "bridge_core.h"
#include "meta_hash.h"
#include "metadata_table.h"
#include <cstring>
#include <vector>

static StratumLifecycleFn g_lifecycle = nullptr;
static bool g_inited = false;

// ── Java -> native callback dispatch ────────────────────────────────────────
static jobject JNICALL native_dispatch(JNIEnv* env, jclass, jstring jkey, jstring jmethod, jobjectArray args) {
    std::string base = stratum_jstring_to_str(env, jkey);
    std::string method = jmethod ? stratum_jstring_to_str(env, jmethod) : "";
    std::string routed = jmethod ? base + "#" + method : base;
    std::shared_ptr<CbRec> cb = find_cb(routed, base);      // shared_ptr keeps the closure alive while it runs
    if (!cb || !cb->fn) { LOGW("nativeDispatch: no callback for %s", routed.c_str()); return nullptr; }

    JniLocalFrame frame(env, 64);
    jsize len = args ? env->GetArrayLength(args) : 0;
    std::vector<StratumVal> av((size_t)len);
    std::vector<std::string> strs((size_t)len);              // one stable buffer per argument
    for (jsize i = 0; i < len; ++i) {
        jobject e = env->GetObjectArrayElement(args, i);
        java_to_val(env, e, av[i], strs[i], false);          // objects: LOCAL refs, valid during the call
    }
    StratumVal out; memset(&out, 0, sizeof(out)); out.tag = STRATUM_V_NULL;
    std::string errbuf;
    try { cb->fn(cb->ud, method.c_str(), av.data(), (size_t)len, &out); }
    catch (const std::exception& e) { errbuf = e.what(); out.tag = STRATUM_V_ERR; out.v.s.ptr = errbuf.data(); out.v.s.len = errbuf.size(); }
    catch (...) { errbuf = "unknown native exception"; out.tag = STRATUM_V_ERR; out.v.s.ptr = errbuf.data(); out.v.s.len = errbuf.size(); }

    if (out.tag >= STRATUM_V_BOOL && out.tag <= STRATUM_V_DOUBLE) return frame.pop(box_value(env, out));
    switch (out.tag) {
        case STRATUM_V_STR:    return frame.pop((jobject)stratum_str_to_jstring(env, (const char*)out.v.s.ptr, out.v.s.len));
        case STRATUM_V_OBJ: {
            jobject g = (jobject)(uintptr_t)out.v.i;
            if (!g) return frame.pop(nullptr);
            jobject l = env->NewLocalRef(g); env->DeleteGlobalRef(g);   // ownership came from the callback
            return frame.pop(l);
        }
        case STRATUM_V_I32ARR: {
            jintArray arr = env->NewIntArray((jsize)out.v.s.len);
            if (arr && out.v.s.len) env->SetIntArrayRegion(arr, 0, (jsize)out.v.s.len, (const jint*)out.v.s.ptr);
            return frame.pop((jobject)arr);
        }
        case STRATUM_V_ERR: {
            std::string m((const char*)out.v.s.ptr, out.v.s.len);
            jclass rex = env->FindClass("java/lang/RuntimeException");
            if (rex) { env->ThrowNew(rex, ("[Stratum] native callback error: " + m).c_str()); env->DeleteLocalRef(rex); }
            LOGE("callback error: %s", m.c_str());
            return frame.pop(nullptr);
        }
        default: return frame.pop(nullptr);
    }
}
static void JNICALL native_release(JNIEnv* env, jclass, jstring jkey) {
    if (!jkey) return;
    std::string k = stratum_jstring_to_str(env, jkey);
    remove_cb(k); remove_prefix(k + "#");
}

// ── Activity lifecycle (natives of the existing StratumActivity.java) ───────
static void life(const char* ev) {
    if (!g_lifecycle) return;
    int64_t act; { std::lock_guard<std::mutex> lk(g_activity_mutex); act = (int64_t)(uintptr_t)g_activity; }
    g_lifecycle(ev, act);                                   // called WITHOUT the lock: user code may call stratum_get_activity()
}
static void JNICALL n_set_activity(JNIEnv* env, jclass, jobject act) {
    std::lock_guard<std::mutex> lk(g_activity_mutex);
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = act ? env->NewGlobalRef(act) : nullptr;
}
static void JNICALL n_create(JNIEnv*, jclass)  { life("onCreate"); }
static void JNICALL n_resume(JNIEnv*, jclass)  { life("onResume"); }
static void JNICALL n_pause(JNIEnv*, jclass)   { life("onPause"); }
static void JNICALL n_stop(JNIEnv*, jclass)    { life("onStop"); }
static void JNICALL n_destroy(JNIEnv* env, jclass) {
    life("onDestroy");
    std::lock_guard<std::mutex> lk(g_activity_mutex);
    if (g_activity) { env->DeleteGlobalRef(g_activity); g_activity = nullptr; }
}
// Individually: a missing Java method must not stop the others from registering.
static void reg1(JNIEnv* env, jclass c, const char* name, const char* sig, void* fn) {
    JNINativeMethod m{(char*)name, (char*)sig, fn};
    if (env->RegisterNatives(c, &m, 1) != 0) { env->ExceptionClear(); LOGW("RegisterNatives: %s%s not found (ok if unused)", name, sig); }
}

extern "C" {

int32_t stratum_abi_version(void) { return STRATUM_ABI_VERSION; }
const char* stratum_meta_hash(void) { return STRATUM_META_HASH; }
const char* stratum_last_error(void) { return t_err.c_str(); }
void* stratum_get_env(void) { return get_env(); }

int32_t stratum_core_init(void* vm_) {
    if (g_inited) return 0;
    JavaVM* vm = (JavaVM*)vm_; g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return -1;
    auto cache = [&](const char* n) -> jclass {
        jclass l = env->FindClass(n);
        if (!l) { env->ExceptionClear(); return nullptr; }
        jclass g = (jclass)env->NewGlobalRef(l); env->DeleteLocalRef(l); return g;
    };
    g_jstring_class = cache("java/lang/String"); g_object_class = cache("java/lang/Object"); g_class_class = cache("java/lang/Class");
    g_handler_class = cache("com/stratum/runtime/StratumInvocationHandler");
    if (!g_handler_class) { LOGE("StratumInvocationHandler not found: copy runtime/java/com/stratum/runtime/*.java into your app"); return -1; }
    jmethodID gcl = env->GetMethodID(g_class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = env->CallObjectMethod(g_handler_class, gcl);
    g_app_class_loader = env->NewGlobalRef(loader);
    jclass lcls = env->FindClass("java/lang/ClassLoader");
    g_loadClass_mid = env->GetMethodID(lcls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(lcls); env->DeleteLocalRef(loader);
    g_handler_ctor = env->GetMethodID(g_handler_class, "<init>", "(Ljava/lang/String;)V");
    g_proxy_class = cache("java/lang/reflect/Proxy");
    g_proxy_new = env->GetStaticMethodID(g_proxy_class, "newProxyInstance",
        "(Ljava/lang/ClassLoader;[Ljava/lang/Class;Ljava/lang/reflect/InvocationHandler;)Ljava/lang/Object;");
    g_obj_tostring = env->GetMethodID(g_object_class, "toString", "()Ljava/lang/String;");
    g_arr_b = cache("[B"); g_arr_i = cache("[I"); g_arr_j = cache("[J"); g_arr_f = cache("[F"); g_arr_d = cache("[D");
    g_arr_z = cache("[Z"); g_arr_c = cache("[C"); g_arr_s = cache("[S"); g_arr_str = cache("[Ljava/lang/String;");
    for (auto& pr : g_prims) {
        pr.c = cache(pr.cls);
        pr.valueOf = env->GetStaticMethodID(pr.c, "valueOf", pr.box_sig);
        pr.unbox = env->GetMethodID(pr.c, pr.unbox_name, pr.unbox_sig);
    }
    reg1(env, g_handler_class, "nativeDispatch", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;", (void*)native_dispatch);
    reg1(env, g_handler_class, "nativeRelease", "(Ljava/lang/String;)V", (void*)native_release);
    jclass ac = env->FindClass("com/stratum/runtime/StratumActivity");
    if (ac) {
        reg1(env, ac, "nativeSetActivity", "(Ljava/lang/Object;)V", (void*)n_set_activity);
        reg1(env, ac, "nativeOnCreate", "()V", (void*)n_create);   reg1(env, ac, "nativeOnResume", "()V", (void*)n_resume);
        reg1(env, ac, "nativeOnPause", "()V", (void*)n_pause);     reg1(env, ac, "nativeOnStop", "()V", (void*)n_stop);
        reg1(env, ac, "nativeOnDestroy", "()V", (void*)n_destroy);
        env->DeleteLocalRef(ac);
    } else env->ExceptionClear();
    g_inited = true;
    LOGI("Stratum core initialized (C ABI v%d). %u classes indexed lazily.", STRATUM_ABI_VERSION, g_class_count);
    return 0;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) { return stratum_core_init(vm) == 0 ? JNI_VERSION_1_6 : JNI_ERR; }

void stratum_set_lifecycle_dispatcher(StratumLifecycleFn f) { g_lifecycle = f; }

void stratum_register_callback(const char* key, const StratumCbDesc* d) {
    if (!key || !d || !d->fn) return;
    StratumCbDesc owned = *d; const_cast<StratumCbDesc*>(d)->userdata = nullptr;
    store_cb(key, owned);
}
void stratum_remove_callback(const char* key) { if (key) remove_cb(key); }
size_t stratum_remove_callbacks_by_prefix(const char* p) { return p ? remove_prefix(p) : 0; }
size_t stratum_callback_count(void) { return cb_count(); }

int32_t stratum_post_ui(const StratumCbDesc* d) {
    int32_t err = 0;
    guarded<int>(&err, 0, [&]() -> int {
        if (!d || !d->fn) throw std::runtime_error("post_ui: empty callback");
        JNIEnv* env = get_env(); if (!env) throw std::runtime_error("Stratum: no JNIEnv");
        JniLocalFrame fr(env, 32);
        static jobject s_handler = nullptr; static std::mutex m;
        { std::lock_guard<std::mutex> lk(m);
          if (!s_handler) {
              jclass lc = find_class(env, "android/os/Looper");
              jmethodID gm = env->GetStaticMethodID(lc, "getMainLooper", "()Landroid/os/Looper;");
              jobject looper = env->CallStaticObjectMethod(lc, gm);
              jclass hc = find_class(env, "android/os/Handler");
              jmethodID ctor = env->GetMethodID(hc, "<init>", "(Landroid/os/Looper;)V");
              jobject h = env->NewObject(hc, ctor, looper);
              stratum_check_java_exc(env);
              s_handler = env->NewGlobalRef(h);
          } }
        StratumCbDesc owned = *d; const_cast<StratumCbDesc*>(d)->userdata = nullptr;
        std::string key = make_key('p'); store_cb(key, owned);
        try {
            jobject proxy = make_proxy_obj(env, "java/lang/Runnable", key);
            jclass hc2 = env->GetObjectClass(s_handler);
            jmethodID post = env->GetMethodID(hc2, "post", "(Ljava/lang/Runnable;)Z");
            env->CallBooleanMethod(s_handler, post, proxy);
            stratum_check_java_exc(env);
        } catch (...) { remove_cb(key); throw; }
        return 0;
    });
    return err;
}

int64_t stratum_get_activity(void) {
    std::lock_guard<std::mutex> lk(g_activity_mutex);
    JNIEnv* env = get_env();
    return (g_activity && env) ? (int64_t)(uintptr_t)env->NewGlobalRef(g_activity) : 0;
}
int64_t stratum_create_view(const char* key) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env) return 0;
        jobject act; { std::lock_guard<std::mutex> lk(g_activity_mutex); act = g_activity; }
        if (!act) throw std::runtime_error("Stratum: Activity not ready");
        JniLocalFrame fr(env, 8);
        jclass cls = find_class(env, "com/stratum/runtime/StratumView");
        if (!cls) throw std::runtime_error("Stratum: StratumView.java not compiled into APK");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
        jstring jk = env->NewStringUTF(key);
        jobject o = env->NewObject(cls, ctor, act, jk);
        stratum_check_java_exc(env);
        return o ? (int64_t)(uintptr_t)env->NewGlobalRef(o) : 0;
    });
}
void stratum_set_content_view(int64_t act_p, int64_t view_p) {
    guarded<int>(nullptr, 0, [&]() -> int {
        if (!act_p || !view_p) return 0;
        JNIEnv* env = get_env(); if (!env) return 0; JniLocalFrame fr(env, 8);
        jobject act = (jobject)(uintptr_t)act_p;
        jclass c = env->GetObjectClass(act);
        jmethodID mid = env->GetMethodID(c, "setContentView", "(Landroid/view/View;)V");
        if (mid) env->CallVoidMethod(act, mid, (jobject)(uintptr_t)view_p);
        stratum_check_java_exc(env); return 0;
    });
}

void stratum_set_log_enabled(uint8_t e) { g_log_enabled = e != 0; }
uint8_t stratum_is_log_enabled(void) { return g_log_build_supported && g_log_enabled; }
void stratum_log_msg(const char* m) { if (m && g_log_build_supported && g_log_enabled) __android_log_print(ANDROID_LOG_INFO, "Stratum", "[app] %s", m); }

} // extern "C"