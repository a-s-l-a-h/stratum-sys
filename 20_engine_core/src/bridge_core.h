#pragma once
#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include "stratum_c_abi.h"

#ifndef STRATUM_LOG_ENABLED
#define STRATUM_LOG_ENABLED 0
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Stratum", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "Stratum", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "Stratum", __VA_ARGS__)
extern bool g_log_enabled;
extern const bool g_log_build_supported;
#if STRATUM_LOG_ENABLED
#define LOGD(...) do { if (g_log_enabled) __android_log_print(ANDROID_LOG_DEBUG,   "Stratum", __VA_ARGS__); } while (0)
#define LOGT(...) do { if (g_log_enabled) __android_log_print(ANDROID_LOG_VERBOSE, "StratumTrace", __VA_ARGS__); } while (0)
#else
#define LOGD(...) ((void)0)
#define LOGT(...) ((void)0)
#endif

struct Prim { char kind; const char* cls; const char* box_sig; const char* unbox_name; const char* unbox_sig;
              jclass c; jmethodID valueOf; jmethodID unbox; };            // order = Z B C S I J F D = tags 1..8

extern JavaVM*   g_jvm;
extern jobject   g_app_class_loader;
extern jmethodID g_loadClass_mid;
extern jclass    g_jstring_class, g_object_class, g_class_class, g_proxy_class, g_handler_class;
extern jmethodID g_proxy_new, g_handler_ctor, g_obj_tostring;
extern jclass    g_arr_b, g_arr_i, g_arr_j, g_arr_f, g_arr_d, g_arr_z, g_arr_c, g_arr_s, g_arr_str;
extern jobject   g_activity;
extern std::mutex g_activity_mutex;
extern Prim      g_prims[8];
extern thread_local std::string t_err;

JNIEnv*     get_env();                                   // attaches on demand, detaches at thread exit
jclass      find_class(JNIEnv* env, const char* name);   // FindClass, then the app ClassLoader
void        stratum_check_java_exc(JNIEnv* env);         // throws std::runtime_error (unwraps InvocationTargetException)
jstring     stratum_str_to_jstring(JNIEnv* env, const char* utf8, size_t len);
std::string stratum_jstring_to_str(JNIEnv* env, jobject jstr);       // does not free the local ref
std::string object_to_std_string(JNIEnv* env, jobject o);            // String or toString()
jobject     box_value(JNIEnv* env, const StratumVal& v);             // local ref
void        java_to_val(JNIEnv* env, jobject e, StratumVal& v, std::string& store, bool make_global);

std::string make_key(char tag);
void   store_cb(const std::string& key, const StratumCbDesc& d);
void   remove_cb(const std::string& key);
size_t remove_prefix(const std::string& prefix);
size_t cb_count();
struct CbRec { void* ud; StratumTramp fn; StratumDropFn drop;
               explicit CbRec(const StratumCbDesc& d) : ud(d.userdata), fn(d.fn), drop(d.drop) {}
               ~CbRec() { if (drop && ud) drop(ud); } };
std::shared_ptr<CbRec> find_cb(const std::string& routed, const std::string& base);

jobject make_adapter_obj(JNIEnv* env, const char* adapter_jni, const std::string& key);
jobject make_proxy_obj(JNIEnv* env, const char* iface_jni, const std::string& key);

struct JniLocalFrame {
    JNIEnv* env_; bool active_;
    explicit JniLocalFrame(JNIEnv* env, jint cap = 32) : env_(env), active_(false) {
        if (env_ && env_->PushLocalFrame(cap) == 0) active_ = true;
    }
    ~JniLocalFrame() { if (active_) env_->PopLocalFrame(nullptr); }
    jobject pop(jobject r) { if (active_) { active_ = false; return env_->PopLocalFrame(r); } return r; }
    JniLocalFrame(const JniLocalFrame&) = delete;
    JniLocalFrame& operator=(const JniLocalFrame&) = delete;
};

template <class R, class F>
inline R guarded(int32_t* err, R fail, F&& body) {
    try { if (err) *err = 0; return body(); }
    catch (const std::exception& e) { t_err = e.what(); }
    catch (...) { t_err = "Stratum: unknown native error"; }
    if (err) *err = 1;
    return fail;
}