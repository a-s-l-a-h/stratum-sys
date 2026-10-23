#include "bridge_core.h"
#include <pthread.h>
#include <cstring>
#include <unordered_map>
#include <vector>

bool g_log_enabled = true;
#if STRATUM_LOG_ENABLED
const bool g_log_build_supported = true;
#else
const bool g_log_build_supported = false;
#endif

JavaVM* g_jvm = nullptr; jobject g_app_class_loader = nullptr; jmethodID g_loadClass_mid = nullptr;
jclass g_jstring_class = nullptr, g_object_class = nullptr, g_class_class = nullptr, g_proxy_class = nullptr, g_handler_class = nullptr;
jmethodID g_proxy_new = nullptr, g_handler_ctor = nullptr, g_obj_tostring = nullptr;
jclass g_arr_b = nullptr, g_arr_i = nullptr, g_arr_j = nullptr, g_arr_f = nullptr, g_arr_d = nullptr,
       g_arr_z = nullptr, g_arr_c = nullptr, g_arr_s = nullptr, g_arr_str = nullptr;
jobject g_activity = nullptr; std::mutex g_activity_mutex;
thread_local std::string t_err;

Prim g_prims[8] = {
    {'Z',"java/lang/Boolean",  "(Z)Ljava/lang/Boolean;",   "booleanValue","()Z",nullptr,nullptr,nullptr},
    {'B',"java/lang/Byte",     "(B)Ljava/lang/Byte;",      "byteValue",   "()B",nullptr,nullptr,nullptr},
    {'C',"java/lang/Character","(C)Ljava/lang/Character;","charValue",   "()C",nullptr,nullptr,nullptr},
    {'S',"java/lang/Short",    "(S)Ljava/lang/Short;",     "shortValue",  "()S",nullptr,nullptr,nullptr},
    {'I',"java/lang/Integer",  "(I)Ljava/lang/Integer;",   "intValue",    "()I",nullptr,nullptr,nullptr},
    {'J',"java/lang/Long",     "(J)Ljava/lang/Long;",      "longValue",   "()J",nullptr,nullptr,nullptr},
    {'F',"java/lang/Float",    "(F)Ljava/lang/Float;",     "floatValue",  "()F",nullptr,nullptr,nullptr},
    {'D',"java/lang/Double",   "(D)Ljava/lang/Double;",    "doubleValue", "()D",nullptr,nullptr,nullptr},
};

// ── thread attach (same design as the Python engine: attach on demand, detach via pthread key) ──
static pthread_key_t  g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void detach_thread(void*) { if (g_jvm) g_jvm->DetachCurrentThread(); }
static void make_tls_key() { pthread_key_create(&g_key, detach_thread); }
static thread_local JNIEnv* t_env = nullptr;

JNIEnv* get_env() {
    if (t_env) return t_env;
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint rc = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        pthread_once(&g_once, make_tls_key);
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        pthread_setspecific(g_key, reinterpret_cast<void*>(1));
    } else if (rc != JNI_OK) return nullptr;
    t_env = env;
    return env;
}

jclass find_class(JNIEnv* env, const char* name) {
    if (!env || !name) return nullptr;
    jclass cls = env->FindClass(name);
    if (cls) return cls;
    env->ExceptionClear();
    if (g_app_class_loader && g_loadClass_mid) {
        std::string dotted = name;
        for (char& c : dotted) if (c == '/') c = '.';
        jstring jn = env->NewStringUTF(dotted.c_str());
        cls = (jclass)env->CallObjectMethod(g_app_class_loader, g_loadClass_mid, jn);
        env->DeleteLocalRef(jn);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return cls;
    }
    return nullptr;
}

void stratum_check_java_exc(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return;
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string msg = "Java exception";
    if (ex) {
        jclass ite = env->FindClass("java/lang/reflect/InvocationTargetException");
        if (env->ExceptionCheck()) { env->ExceptionClear(); ite = nullptr; }
        if (ite && env->IsInstanceOf(ex, ite)) {
            jmethodID m = env->GetMethodID(ite, "getTargetException", "()Ljava/lang/Throwable;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }
            if (m) {
                jthrowable c = (jthrowable)env->CallObjectMethod(ex, m);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else if (c) { env->DeleteLocalRef(ex); ex = c; }
            }
        }
        if (ite) env->DeleteLocalRef(ite);
        jclass ec = env->GetObjectClass(ex);
        jmethodID ts = env->GetMethodID(ec, "toString", "()Ljava/lang/String;");
        jstring js = ts ? (jstring)env->CallObjectMethod(ex, ts) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); js = nullptr; }
        if (js) { msg = stratum_jstring_to_str(env, js); env->DeleteLocalRef(js); }
        env->DeleteLocalRef(ec); env->DeleteLocalRef(ex);
    }
    LOGE("Java exception: %s", msg.c_str());
    throw std::runtime_error(msg);
}

// Hand-written UTF-8 <-> UTF-16 (emoji / non-BMP safe). Unchanged algorithm from the Python engine.
jstring stratum_str_to_jstring(JNIEnv* env, const char* utf8, size_t ul) {
    if (!env || !utf8) return nullptr;
    const uint8_t* ub = reinterpret_cast<const uint8_t*>(utf8);
    std::u16string u16; u16.reserve(ul);
    for (size_t i = 0; i < ul; ) {
        uint32_t cp; uint8_t b0 = ub[i];
        if (b0 < 0x80u) { cp = b0; i += 1; }
        else if ((b0 & 0xE0u) == 0xC0u && i + 1 < ul) { cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (ub[i+1] & 0x3Fu); i += 2; }
        else if ((b0 & 0xF0u) == 0xE0u && i + 2 < ul) { cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(ub[i+1] & 0x3Fu) << 6) | (ub[i+2] & 0x3Fu); i += 3; }
        else if ((b0 & 0xF8u) == 0xF0u && i + 3 < ul) { cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(ub[i+1] & 0x3Fu) << 12) | ((uint32_t)(ub[i+2] & 0x3Fu) << 6) | (ub[i+3] & 0x3Fu); i += 4; }
        else { ++i; continue; }
        if (cp < 0x10000u) u16 += (char16_t)cp;
        else { cp -= 0x10000u; u16 += (char16_t)(0xD800u | (cp >> 10)); u16 += (char16_t)(0xDC00u | (cp & 0x3FFu)); }
    }
    return env->NewString(reinterpret_cast<const jchar*>(u16.data()), (jsize)u16.size());
}

std::string stratum_jstring_to_str(JNIEnv* env, jobject jstr_obj) {
    if (!env || !jstr_obj) return "";
    jstring jstr = (jstring)jstr_obj;
    jsize slen = env->GetStringLength(jstr);
    const jchar* sjc = env->GetStringChars(jstr, nullptr);
    std::string res;
    if (sjc && slen > 0) {
        res.reserve((size_t)slen * 3);
        for (jsize i = 0; i < slen; ) {
            uint32_t cp; uint16_t c1 = (uint16_t)sjc[i++];
            if (c1 >= 0xD800u && c1 <= 0xDBFFu && i < slen) {
                uint16_t c2 = (uint16_t)sjc[i];
                if (c2 >= 0xDC00u && c2 <= 0xDFFFu) { cp = 0x10000u + (((uint32_t)(c1 - 0xD800u)) << 10) + (uint32_t)(c2 - 0xDC00u); ++i; }
                else cp = c1;
            } else cp = c1;
            if (cp < 0x80u) res += (char)cp;
            else if (cp < 0x800u) { res += (char)(0xC0u | (cp >> 6)); res += (char)(0x80u | (cp & 0x3Fu)); }
            else if (cp < 0x10000u) { res += (char)(0xE0u | (cp >> 12)); res += (char)(0x80u | ((cp >> 6) & 0x3Fu)); res += (char)(0x80u | (cp & 0x3Fu)); }
            else { res += (char)(0xF0u | (cp >> 18)); res += (char)(0x80u | ((cp >> 12) & 0x3Fu)); res += (char)(0x80u | ((cp >> 6) & 0x3Fu)); res += (char)(0x80u | (cp & 0x3Fu)); }
        }
    }
    if (sjc) env->ReleaseStringChars(jstr, sjc);
    return res;
}

std::string object_to_std_string(JNIEnv* env, jobject o) {
    if (!o) return "";
    if (g_jstring_class && env->IsInstanceOf(o, g_jstring_class)) return stratum_jstring_to_str(env, o);
    jstring js = (jstring)env->CallObjectMethod(o, g_obj_tostring);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    return js ? stratum_jstring_to_str(env, js) : "";
}

jobject box_value(JNIEnv* env, const StratumVal& v) {
    switch (v.tag) {
        case STRATUM_V_NULL: return nullptr;
        case STRATUM_V_STR:  return v.v.s.ptr ? (jobject)stratum_str_to_jstring(env, (const char*)v.v.s.ptr, v.v.s.len) : nullptr;
        case STRATUM_V_OBJ:  return v.v.i ? env->NewLocalRef((jobject)(uintptr_t)v.v.i) : nullptr;
        default: break;
    }
    if (v.tag < STRATUM_V_BOOL || v.tag > STRATUM_V_DOUBLE) return nullptr;
    Prim& pr = g_prims[v.tag - 1]; jvalue jv; jv.j = 0;
    switch (pr.kind) {
        case 'Z': jv.z = v.v.i ? JNI_TRUE : JNI_FALSE; break; case 'B': jv.b = (jbyte)v.v.i; break;
        case 'C': jv.c = (jchar)v.v.i; break;              case 'S': jv.s = (jshort)v.v.i; break;
        case 'I': jv.i = (jint)v.v.i; break;               case 'J': jv.j = (jlong)v.v.i; break;
        case 'F': jv.f = (jfloat)v.v.d; break;             case 'D': jv.d = v.v.d; break;
    }
    return env->CallStaticObjectMethodA(pr.c, pr.valueOf, &jv);
}

void java_to_val(JNIEnv* env, jobject e, StratumVal& v, std::string& store, bool global) {
    memset(&v, 0, sizeof(v)); v.tag = STRATUM_V_NULL;
    if (!e) return;
    if (g_jstring_class && env->IsInstanceOf(e, g_jstring_class)) {
        store = stratum_jstring_to_str(env, e);
        v.tag = STRATUM_V_STR; v.v.s.ptr = store.data(); v.v.s.len = store.size(); return;
    }
    for (int k = 0; k < 8; ++k) {
        Prim& pr = g_prims[k];
        if (!pr.c || !env->IsInstanceOf(e, pr.c)) continue;
        v.tag = (uint8_t)(k + 1);
        switch (pr.kind) {
            case 'Z': v.v.i = env->CallBooleanMethod(e, pr.unbox) != JNI_FALSE; break;
            case 'B': v.v.i = env->CallByteMethod(e, pr.unbox); break;
            case 'C': v.v.i = env->CallCharMethod(e, pr.unbox); break;
            case 'S': v.v.i = env->CallShortMethod(e, pr.unbox); break;
            case 'I': v.v.i = env->CallIntMethod(e, pr.unbox); break;
            case 'J': v.v.i = env->CallLongMethod(e, pr.unbox); break;
            case 'F': v.v.d = env->CallFloatMethod(e, pr.unbox); break;
            case 'D': v.v.d = env->CallDoubleMethod(e, pr.unbox); break;
        }
        return;
    }
    v.tag = STRATUM_V_OBJ;
    v.v.i = (int64_t)(uintptr_t)(global ? env->NewGlobalRef(e) : e);
}

// ── callback registry ──
static std::unordered_map<std::string, std::shared_ptr<CbRec>> g_cbs;
static std::mutex g_cb_mutex;
std::string make_key(char tag) { static std::atomic<uint64_t> id{0}; return std::string("cb_") + tag + "_" + std::to_string(++id); }
// CbRec destructors run user code (closure drop): always outside the lock.
void store_cb(const std::string& key, const StratumCbDesc& d) {
    auto rec = std::make_shared<CbRec>(d); std::shared_ptr<CbRec> old;
    std::lock_guard<std::mutex> lk(g_cb_mutex);
    auto it = g_cbs.find(key);
    if (it != g_cbs.end()) { old = it->second; it->second = rec; } else g_cbs.emplace(key, rec);
    // `old` dies after the lock is released? no: declared before lk, so destroyed AFTER lk. OK.
}
void remove_cb(const std::string& key) {
    std::shared_ptr<CbRec> dead;
    std::lock_guard<std::mutex> lk(g_cb_mutex);
    auto it = g_cbs.find(key);
    if (it != g_cbs.end()) { dead = it->second; g_cbs.erase(it); }
}
size_t remove_prefix(const std::string& pfx) {
    std::vector<std::shared_ptr<CbRec>> dead;
    std::lock_guard<std::mutex> lk(g_cb_mutex);
    for (auto it = g_cbs.begin(); it != g_cbs.end();) {
        if (it->first.compare(0, pfx.size(), pfx) == 0) { dead.push_back(it->second); it = g_cbs.erase(it); } else ++it;
    }
    return dead.size();
}
size_t cb_count() { std::lock_guard<std::mutex> lk(g_cb_mutex); return g_cbs.size(); }
std::shared_ptr<CbRec> find_cb(const std::string& routed, const std::string& base) {
    std::lock_guard<std::mutex> lk(g_cb_mutex);
    auto it = g_cbs.find(routed);
    if (it == g_cbs.end()) it = g_cbs.find(base);
    return it == g_cbs.end() ? nullptr : it->second;
}

jobject make_adapter_obj(JNIEnv* env, const char* name, const std::string& key) {
    jclass acls = find_class(env, name);
    if (!acls) throw std::runtime_error(std::string("Stratum: adapter class not compiled into APK: ") + name);
    jmethodID ctor = env->GetMethodID(acls, "<init>", "(Ljava/lang/String;)V");
    if (!ctor) { env->ExceptionClear(); env->DeleteLocalRef(acls); throw std::runtime_error(std::string("Stratum: adapter has no (String) ctor: ") + name); }
    jstring jk = env->NewStringUTF(key.c_str());
    jobject o = env->NewObject(acls, ctor, jk);
    env->DeleteLocalRef(jk); env->DeleteLocalRef(acls);
    stratum_check_java_exc(env);
    return o;
}
jobject make_proxy_obj(JNIEnv* env, const char* name, const std::string& key) {
    jclass icls = find_class(env, name);
    if (!icls) throw std::runtime_error(std::string("Stratum: interface not found: ") + name);
    jstring jk = env->NewStringUTF(key.c_str());
    jobject handler = env->NewObject(g_handler_class, g_handler_ctor, jk);
    env->DeleteLocalRef(jk);
    jobjectArray ia = env->NewObjectArray(1, g_class_class, icls);
    jobject proxy = env->CallStaticObjectMethod(g_proxy_class, g_proxy_new, g_app_class_loader, ia, handler);
    env->DeleteLocalRef(icls); env->DeleteLocalRef(ia); env->DeleteLocalRef(handler);
    stratum_check_java_exc(env);
    return proxy;
}