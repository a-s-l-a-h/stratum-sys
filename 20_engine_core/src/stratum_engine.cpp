// The dispatcher: (class_id, slot) -> JNI, driven by Stage 05 param_tags. Same design as the Python engine minus nanobind.
#include "stratum_c_abi.h"
#include "bridge_core.h"
#include "metadata_table.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <cstdlib>
#include <cstring>
#include <vector>

static inline const char* get_str(uint32_t off) { return &g_str_pool[off]; }
static thread_local std::string t_buf;
static std::recursive_mutex g_resolve_mutex;   // recursive: <clinit> may re-enter resolve on the same thread

static void resolve_class(JNIEnv* env, uint32_t cid) {
    if (cid >= g_class_count) throw std::runtime_error("Stratum: class_id out of range");
    std::lock_guard<std::recursive_mutex> lock(g_resolve_mutex);
    ClassMeta& cls = g_classes[cid];
    if (cls.resolved.load(std::memory_order_acquire)) return;
    const char* jni_name = get_str(cls.jni_name_offset);
    jclass local = find_class(env, jni_name);
    if (!local) { env->ExceptionClear(); throw std::runtime_error(std::string("Stratum: class not found on this device: ") + jni_name); }
    cls.class_ref = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    LOGD("resolved class %s (class_id=%u)", jni_name, cid);
    if (cls.method_count > 0) {
        cls.method_ids = new jmethodID[cls.method_count];
        for (uint16_t i = 0; i < cls.method_count; ++i) {
            const MethodMeta& mm = cls.methods[i];
            const char* mname = mm.is_constructor ? "<init>" : get_str(mm.name_offset);
            const char* msig = get_str(mm.sig_offset);
            cls.method_ids[i] = mm.is_static ? env->GetStaticMethodID(cls.class_ref, mname, msig)
                                             : env->GetMethodID(cls.class_ref, mname, msig);
            if (!cls.method_ids[i]) { env->ExceptionClear(); LOGW("method unavailable on this device: %s#%s%s", jni_name, mname, msig); }
        }
    }
    if (cls.field_count > 0) {
        cls.field_ids = new jfieldID[cls.field_count];
        for (uint16_t i = 0; i < cls.field_count; ++i) {
            const FieldMeta& fm = cls.fields[i];
            const char* fname = get_str(fm.name_offset); const char* fsig = get_str(fm.sig_offset);
            cls.field_ids[i] = fm.is_static ? env->GetStaticFieldID(cls.class_ref, fname, fsig)
                                            : env->GetFieldID(cls.class_ref, fname, fsig);
            if (!cls.field_ids[i]) { env->ExceptionClear(); LOGW("field unavailable: %s#%s", jni_name, fname); }
        }
    }
    cls.resolved.store(true, std::memory_order_release);
}

#define PACK_ARR(TAG, JT, NEWF, SETF, CT)                                                   \
    case TAG: {                                                                              \
        const StratumSlice* s = (const StratumSlice*)args[i].p;                              \
        if (!s || (!s->ptr && s->len)) { jv[i].l = nullptr; break; }                         \
        JT arr = env->NEWF((jsize)s->len);                                                   \
        if (!arr) { stratum_check_java_exc(env); throw std::runtime_error("Stratum: OOM allocating array"); } \
        if (s->len) env->SETF(arr, 0, (jsize)s->len, (const CT*)s->ptr);                     \
        jv[i].l = arr; break; }

// Descriptor of parameter `idx` inside "(...)R": "I", "Landroid/view/Surface;", "[[I", ...
static std::string param_desc(const char* sig, int idx) {
    if (!sig || *sig != '(') return "";
    const char* p = sig + 1;
    for (int k = 0; *p && *p != ')'; ++k) {
        const char* s = p;
        while (*p == '[') ++p;
        if (*p == 'L') { while (*p && *p != ';') ++p; }
        if (*p) ++p;
        if (k == idx) return std::string(s, (size_t)(p - s));
    }
    return "";
}

static void pack(JNIEnv* env, const char* tags, const MethodMeta& mm, const StratumArg* args, jvalue* jv) {
    for (uint8_t i = 0; i < mm.param_count; ++i) {
        switch (tags[i]) {
            case 'Z': jv[i].z = args[i].i ? JNI_TRUE : JNI_FALSE; break;
            case 'B': jv[i].b = (jbyte)args[i].i; break;
            case 'C': jv[i].c = (jchar)args[i].i; break;
            case 'S': jv[i].s = (jshort)args[i].i; break;
            case 'I': jv[i].i = (jint)args[i].i; break;
            case 'J': jv[i].j = (jlong)args[i].i; break;
            case 'F': jv[i].f = args[i].f; break;
            case 'D': jv[i].d = args[i].d; break;
            case 's': {
                const StratumStr* s = (const StratumStr*)args[i].p;
                jv[i].l = (!s || !s->ptr) ? nullptr : stratum_str_to_jstring(env, s->ptr, s->len);
                break;
            }
            PACK_ARR('[', jbyteArray,    NewByteArray,    SetByteArrayRegion,    jbyte)
            PACK_ARR(']', jintArray,     NewIntArray,     SetIntArrayRegion,     jint)
            PACK_ARR('q', jlongArray,    NewLongArray,    SetLongArrayRegion,    jlong)
            PACK_ARR('f', jfloatArray,   NewFloatArray,   SetFloatArrayRegion,   jfloat)
            PACK_ARR('d', jdoubleArray,  NewDoubleArray,  SetDoubleArrayRegion,  jdouble)
            PACK_ARR('b', jbooleanArray, NewBooleanArray, SetBooleanArrayRegion, jboolean)
            PACK_ARR('c', jcharArray,    NewCharArray,    SetCharArrayRegion,    jchar)
            PACK_ARR('h', jshortArray,   NewShortArray,   SetShortArrayRegion,   jshort)
            case 'T': {
                const StratumSlice* s = (const StratumSlice*)args[i].p;
                if (!s) { jv[i].l = nullptr; break; }
                jobjectArray arr = env->NewObjectArray((jsize)s->len, g_jstring_class, nullptr);
                const StratumStr* items = (const StratumStr*)s->ptr;
                for (size_t k = 0; k < s->len; ++k) {
                    if (!items[k].ptr) continue;
                    jstring js = stratum_str_to_jstring(env, items[k].ptr, items[k].len);
                    env->SetObjectArrayElement(arr, (jsize)k, js);
                    env->DeleteLocalRef(js);
                }
                jv[i].l = arr; break;
            }
            case 'A': {
                const StratumSlice* s = (const StratumSlice*)args[i].p;
                if (!s || (!s->ptr && s->len)) { jv[i].l = nullptr; break; }
                // Element class comes from the real method descriptor: "[Lx/Y;" -> x/Y, "[[I" -> "[I" (int[])
                jclass ec = g_object_class, local = nullptr;
                std::string d = param_desc(get_str(mm.sig_offset), (int)i);
                if (d.size() > 1 && d[0] == '[') {
                    std::string e = d.substr(1);
                    if (e.size() >= 2 && e.front() == 'L' && e.back() == ';') e = e.substr(1, e.size() - 2);
                    else if (e.empty() || e.front() != '[') e.clear();       // primitive/malformed: keep Object[]
                    if (!e.empty() && e != "java/lang/Object") {            // Object[] needs no lookup
                        local = find_class(env, e.c_str());
                        if (local) ec = local;                              // unknown class: old behaviour
                    }
                }
                jobjectArray arr = env->NewObjectArray((jsize)s->len, ec, nullptr);
                if (local) env->DeleteLocalRef(local);
                if (!arr) { stratum_check_java_exc(env); throw std::runtime_error("Stratum: OOM allocating array"); }
                const int64_t* items = (const int64_t*)s->ptr;
                for (size_t k = 0; k < s->len; ++k) {
                    if (items && items[k]) {
                        env->SetObjectArrayElement(arr, (jsize)k, (jobject)(uintptr_t)items[k]);
                        stratum_check_java_exc(env);                        // wrong element type -> clear error
                    }
                }
                jv[i].l = arr; break;
            }
            case 'M': {
                const StratumListArg* l = (const StratumListArg*)args[i].p;
                if (!l) { jv[i].l = nullptr; break; }
                if (l->existing) { jv[i].l = (jobject)(uintptr_t)l->existing; break; }
                jclass ac = find_class(env, "java/util/ArrayList");
                jmethodID ctor = env->GetMethodID(ac, "<init>", "()V");
                jmethodID add = env->GetMethodID(ac, "add", "(Ljava/lang/Object;)Z");
                jobject list = env->NewObject(ac, ctor);
                for (size_t k = 0; k < l->len; ++k) env->CallBooleanMethod(list, add, (jobject)(uintptr_t)l->ptrs[k]);
                env->DeleteLocalRef(ac);
                stratum_check_java_exc(env);
                jv[i].l = list; break;
            }
            case 'N': {
                const StratumMapArg* m = (const StratumMapArg*)args[i].p;
                if (!m) { jv[i].l = nullptr; break; }
                if (m->existing) { jv[i].l = (jobject)(uintptr_t)m->existing; break; }
                jclass hc = find_class(env, "java/util/HashMap");
                jmethodID ctor = env->GetMethodID(hc, "<init>", "()V");
                jmethodID put = env->GetMethodID(hc, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
                jobject map = env->NewObject(hc, ctor);
                for (size_t k = 0; k < m->len; ++k) {
                    jobject prev = env->CallObjectMethod(map, put, (jobject)(uintptr_t)m->keys[k], (jobject)(uintptr_t)m->vals[k]);
                    if (prev) env->DeleteLocalRef(prev);
                }
                env->DeleteLocalRef(hc);
                stratum_check_java_exc(env);
                jv[i].l = map; break;
            }
            case 'a': case 'p': {
                const StratumCbDesc* d = (const StratumCbDesc*)args[i].p;
                if (!d) { jv[i].l = nullptr; break; }
                if (d->existing) { jv[i].l = (jobject)(uintptr_t)d->existing; break; }
                if (!d->fn) { jv[i].l = nullptr; break; }
                StratumCbDesc owned = *d;
                const_cast<StratumCbDesc*>(d)->userdata = nullptr;      // engine owns the closure from here
                std::string key = make_key(tags[i]);
                store_cb(key, owned);
                try {
                    const char* tgt = get_str(mm.adapter_jni_offset);
                    jv[i].l = tags[i] == 'a' ? make_adapter_obj(env, tgt, key) : make_proxy_obj(env, tgt, key);
                } catch (...) { remove_cb(key); throw; }
                break;           // freed by Java finalize() -> nativeRelease(key)
            }
            default: jv[i].l = (jobject)(uintptr_t)args[i].i; break;     // 'L'
        }
    }
}

struct Ctx {
    JNIEnv* env; JniLocalFrame frame; ClassMeta* cls; const MethodMeta* mm;
    jmethodID mid; jobject target; bool is_static; jvalue jv[32];
    Ctx(int64_t ptr, uint32_t cid, uint32_t slot, const StratumArg* args, bool ctor = false)
        : env(get_env()), frame(env, 64) {
        if (!env) throw std::runtime_error("Stratum: no JNIEnv on this thread");
        if (cid >= g_class_count) throw std::runtime_error("Stratum: invalid class_id");
        cls = &g_classes[cid];
        if (!cls->resolved.load(std::memory_order_acquire)) resolve_class(env, cid);
        if (!cls->method_ids || slot >= cls->method_count) throw std::runtime_error("Stratum: invalid method slot");
        mid = cls->method_ids[slot];
        if (!mid) throw std::runtime_error("Stratum: method unavailable on this device API level");
        mm = &cls->methods[slot];
        if (mm->param_count > 32) throw std::runtime_error("Stratum: more than 32 parameters not supported");
        is_static = mm->is_static;
        LOGT(">> CALL [%s#%s] ptr=0x%llx static=%d", get_str(cls->jni_name_offset), get_str(mm->name_offset), (unsigned long long)ptr, (int)is_static);
        target = (jobject)(uintptr_t)ptr;
        if (!ctor && !is_static && !ptr)
            throw std::runtime_error(std::string("Stratum: call on null Java object: ")
                                     + get_str(cls->jni_name_offset) + "#" + get_str(mm->name_offset));
        memset(jv, 0, sizeof(jv));
        pack(env, get_str(mm->tags_offset), *mm, args, jv);
    }
};
#define CALL(N) (c.is_static ? c.env->CallStatic##N##MethodA(c.cls->class_ref, c.mid, c.jv) : c.env->Call##N##MethodA(c.target, c.mid, c.jv))

static void fill_arr(JNIEnv* env, jobject res, StratumArr* out) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    if (!res) return;
    jsize n = env->GetArrayLength((jarray)res);
    out->len = (size_t)n;
    auto isa = [&](jclass k) { return k && env->IsInstanceOf(res, k); };
#define PRIM_ARR(K, CLS, JT, GET, CT) \
    if (isa(CLS)) { out->kind = K; if (n) { CT* b = (CT*)malloc(sizeof(CT) * n); env->GET((JT)res, 0, n, b); out->data = b; } return; }
    PRIM_ARR('B', g_arr_b, jbyteArray,    GetByteArrayRegion,    jbyte)
    PRIM_ARR('I', g_arr_i, jintArray,     GetIntArrayRegion,     jint)
    PRIM_ARR('J', g_arr_j, jlongArray,    GetLongArrayRegion,    jlong)
    PRIM_ARR('F', g_arr_f, jfloatArray,   GetFloatArrayRegion,   jfloat)
    PRIM_ARR('D', g_arr_d, jdoubleArray,  GetDoubleArrayRegion,  jdouble)
    PRIM_ARR('Z', g_arr_z, jbooleanArray, GetBooleanArrayRegion, jboolean)
    PRIM_ARR('C', g_arr_c, jcharArray,    GetCharArrayRegion,    jchar)
    PRIM_ARR('S', g_arr_s, jshortArray,   GetShortArrayRegion,   jshort)
#undef PRIM_ARR
    if (isa(g_arr_str)) {
        out->kind = 'T';
        StratumStr* items = n ? (StratumStr*)calloc(n, sizeof(StratumStr)) : nullptr;
        for (jsize k = 0; k < n; ++k) {
            jobject e = env->GetObjectArrayElement((jobjectArray)res, k);
            if (!e) continue;
            std::string s = stratum_jstring_to_str(env, e);
            char* p = (char*)malloc(s.size() ? s.size() : 1);
            memcpy(p, s.data(), s.size());
            items[k].ptr = p; items[k].len = s.size();
            env->DeleteLocalRef(e);
        }
        out->data = items; return;
    }
    out->kind = 'O';
    int64_t* items = n ? (int64_t*)calloc(n, sizeof(int64_t)) : nullptr;
    for (jsize k = 0; k < n; ++k) {
        jobject e = env->GetObjectArrayElement((jobjectArray)res, k);
        if (e) { items[k] = (int64_t)(uintptr_t)env->NewGlobalRef(e); env->DeleteLocalRef(e); }
    }
    out->data = items;
}

extern "C" {

int64_t stratum_new_instance(uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        Ctx c(0, cid, slot, a, true);
        jobject o = c.env->NewObjectA(c.cls->class_ref, c.mid, c.jv);
        stratum_check_java_exc(c.env);
        return o ? (int64_t)(uintptr_t)c.env->NewGlobalRef(o) : 0;
    });
}
void stratum_call_v(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int {
        Ctx c(p, cid, slot, a);
        if (c.is_static) c.env->CallStaticVoidMethodA(c.cls->class_ref, c.mid, c.jv);
        else c.env->CallVoidMethodA(c.target, c.mid, c.jv);
        stratum_check_java_exc(c.env); return 0;
    });
}
uint8_t stratum_call_z(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<uint8_t>(err, 0, [&]() -> uint8_t { Ctx c(p, cid, slot, a); jboolean r = CALL(Boolean); stratum_check_java_exc(c.env); return r != JNI_FALSE; });
}
int64_t stratum_call_i(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        Ctx c(p, cid, slot, a); int64_t r;
        switch (c.mm->ret_type) {
            case 2: r = (int64_t)CALL(Byte); break;  case 3: r = (int64_t)CALL(Char); break;
            case 4: r = (int64_t)CALL(Short); break; default: r = (int64_t)CALL(Int); break;
        }
        stratum_check_java_exc(c.env); return r;
    });
}
int64_t stratum_call_j(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t { Ctx c(p, cid, slot, a); jlong r = CALL(Long); stratum_check_java_exc(c.env); return r; });
}
double stratum_call_d(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<double>(err, 0.0, [&]() -> double {
        Ctx c(p, cid, slot, a);
        double r = (c.mm->ret_type == 7) ? (double)CALL(Float) : (double)CALL(Double);
        stratum_check_java_exc(c.env); return r;
    });
}
StratumStr stratum_call_str(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<StratumStr>(err, StratumStr{nullptr, 0}, [&]() -> StratumStr {
        Ctx c(p, cid, slot, a); jobject r = CALL(Object); stratum_check_java_exc(c.env);
        if (!r) return StratumStr{nullptr, 0};
        t_buf = object_to_std_string(c.env, r);          // also handles CharSequence returns
        return StratumStr{t_buf.data(), t_buf.size()};
    });
}
int64_t stratum_call_o(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        Ctx c(p, cid, slot, a); jobject r = CALL(Object); stratum_check_java_exc(c.env);
        return r ? (int64_t)(uintptr_t)c.env->NewGlobalRef(r) : 0;
    });
}
void stratum_call_arr(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, StratumArr* out, int32_t* err) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int { Ctx c(p, cid, slot, a); jobject r = CALL(Object); stratum_check_java_exc(c.env); fill_arr(c.env, r, out); return 0; });
}
void stratum_call_list(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, StratumArr* out, int32_t* err) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int {
        Ctx c(p, cid, slot, a); jobject r = CALL(Object); stratum_check_java_exc(c.env);
        if (!r) return 0;
        jclass cc = find_class(c.env, "java/util/Collection");
        jmethodID ta = c.env->GetMethodID(cc, "toArray", "()[Ljava/lang/Object;");
        jobject arr = c.env->CallObjectMethod(r, ta);
        stratum_check_java_exc(c.env);
        if (arr) {
            jsize n = c.env->GetArrayLength((jarray)arr);
            int64_t* items = n ? (int64_t*)calloc((size_t)n, sizeof(int64_t)) : nullptr;
            for (jsize k = 0; k < n; ++k) {
                jobject e = c.env->GetObjectArrayElement((jobjectArray)arr, k);
                if (e) { items[k] = (int64_t)(uintptr_t)c.env->NewGlobalRef(e); c.env->DeleteLocalRef(e); }
            }
            out->kind = 'O'; out->len = (size_t)n; out->data = items;
        }
        return 0;
    });
}

// ── fields ──
struct FCtx {
    JNIEnv* env; JniLocalFrame frame; ClassMeta* cls; const FieldMeta* fm; jfieldID fid; jobject target; bool is_static;
    FCtx(int64_t ptr, uint32_t cid, uint32_t slot) : env(get_env()), frame(env, 16) {
        if (!env) throw std::runtime_error("Stratum: no JNIEnv on this thread");
        if (cid >= g_class_count) throw std::runtime_error("Stratum: invalid class_id");
        cls = &g_classes[cid];
        if (!cls->resolved.load(std::memory_order_acquire)) resolve_class(env, cid);
        if (!cls->field_ids || slot >= cls->field_count) throw std::runtime_error("Stratum: invalid field slot");
        fid = cls->field_ids[slot];
        if (!fid) throw std::runtime_error("Stratum: field unavailable on this device API level");
        fm = &cls->fields[slot]; is_static = fm->is_static; target = (jobject)(uintptr_t)ptr;
        if (!is_static && !ptr) throw std::runtime_error("Stratum: field access on null Java object");
    }
};
#define FGET(N) (f.is_static ? f.env->GetStatic##N##Field(f.cls->class_ref, f.fid) : f.env->Get##N##Field(f.target, f.fid))
#define FSET(N, V) do { if (f.is_static) f.env->SetStatic##N##Field(f.cls->class_ref, f.fid, V); else f.env->Set##N##Field(f.target, f.fid, V); } while (0)

int64_t stratum_field_get_i(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        FCtx f(p, cid, slot); int64_t r;
        switch (f.fm->type_id) { case 2: r = FGET(Byte); break; case 3: r = FGET(Char); break; case 4: r = FGET(Short); break; default: r = FGET(Int); break; }
        stratum_check_java_exc(f.env); return r;
    });
}
uint8_t stratum_field_get_z(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<uint8_t>(err, 0, [&]() -> uint8_t { FCtx f(p, cid, slot); jboolean r = FGET(Boolean); stratum_check_java_exc(f.env); return r != JNI_FALSE; });
}
int64_t stratum_field_get_j(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t { FCtx f(p, cid, slot); jlong r = FGET(Long); stratum_check_java_exc(f.env); return r; });
}
double stratum_field_get_d(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<double>(err, 0.0, [&]() -> double {
        FCtx f(p, cid, slot);
        double r = f.fm->type_id == 7 ? (double)FGET(Float) : (double)FGET(Double);
        stratum_check_java_exc(f.env); return r;
    });
}
StratumStr stratum_field_get_str(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<StratumStr>(err, StratumStr{nullptr, 0}, [&]() -> StratumStr {
        FCtx f(p, cid, slot); jobject r = FGET(Object); stratum_check_java_exc(f.env);
        if (!r) return StratumStr{nullptr, 0};
        t_buf = object_to_std_string(f.env, r); return StratumStr{t_buf.data(), t_buf.size()};
    });
}
int64_t stratum_field_get_o(int64_t p, uint32_t cid, uint32_t slot, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        FCtx f(p, cid, slot); jobject r = FGET(Object); stratum_check_java_exc(f.env);
        return r ? (int64_t)(uintptr_t)f.env->NewGlobalRef(r) : 0;
    });
}
void stratum_field_get_arr(int64_t p, uint32_t cid, uint32_t slot, StratumArr* out, int32_t* err) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int { FCtx f(p, cid, slot); jobject r = FGET(Object); stratum_check_java_exc(f.env); fill_arr(f.env, r, out); return 0; });
}
void stratum_field_set_i(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int {
        FCtx f(p, cid, slot);
        switch (f.fm->type_id) { case 2: FSET(Byte, (jbyte)v); break; case 3: FSET(Char, (jchar)v); break; case 4: FSET(Short, (jshort)v); break; default: FSET(Int, (jint)v); break; }
        stratum_check_java_exc(f.env); return 0;
    });
}
void stratum_field_set_z(int64_t p, uint32_t cid, uint32_t slot, uint8_t v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int { FCtx f(p, cid, slot); FSET(Boolean, v ? JNI_TRUE : JNI_FALSE); stratum_check_java_exc(f.env); return 0; });
}
void stratum_field_set_j(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int { FCtx f(p, cid, slot); FSET(Long, (jlong)v); stratum_check_java_exc(f.env); return 0; });
}
void stratum_field_set_d(int64_t p, uint32_t cid, uint32_t slot, double v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int {
        FCtx f(p, cid, slot);
        if (f.fm->type_id == 7) FSET(Float, (jfloat)v); else FSET(Double, (jdouble)v);
        stratum_check_java_exc(f.env); return 0;
    });
}
void stratum_field_set_str(int64_t p, uint32_t cid, uint32_t slot, const StratumStr* v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int {
        FCtx f(p, cid, slot);
        jstring js = (!v || !v->ptr) ? nullptr : stratum_str_to_jstring(f.env, v->ptr, v->len);
        FSET(Object, js); stratum_check_java_exc(f.env); return 0;
    });
}
void stratum_field_set_o(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err) {
    guarded<int>(err, 0, [&]() -> int { FCtx f(p, cid, slot); FSET(Object, (jobject)(uintptr_t)v); stratum_check_java_exc(f.env); return 0; });
}

// ── refs / helpers ──
void stratum_arr_free(StratumArr* a) {
    if (!a || !a->data) return;
    if (a->kind == 'T') { StratumStr* it = (StratumStr*)a->data; for (size_t k = 0; k < a->len; ++k) free((void*)it[k].ptr); }
    else if (a->kind == 'O') { JNIEnv* env = get_env(); if (env) { int64_t* h = (int64_t*)a->data; for (size_t k = 0; k < a->len; ++k) if (h[k]) env->DeleteGlobalRef((jobject)(uintptr_t)h[k]); } }
    free(a->data); a->data = nullptr; a->len = 0;
}
// NOTE for kind 'O': handles are OWNED by the caller. C++ SDK adopts them into Objects, so it
// copies/clears the array itself and never calls stratum_arr_free on kind 'O' after adopting.
void stratum_get_array_data(int64_t arr_handle, StratumArr* out, int32_t* err) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int {
        if (!arr_handle) return 0;
        JNIEnv* env = get_env();
        if (!env) return 0;
        fill_arr(env, (jobject)(uintptr_t)arr_handle, out);
        return 0;
    });
}

void stratum_delete_ref(int64_t p) { if (!p) return; JNIEnv* env = get_env(); if (env) env->DeleteGlobalRef((jobject)(uintptr_t)p); }
int64_t stratum_clone_ref(int64_t p) { if (!p) return 0; JNIEnv* env = get_env(); return env ? (int64_t)(uintptr_t)env->NewGlobalRef((jobject)(uintptr_t)p) : 0; }
uint8_t stratum_is_instance_of(int64_t p, uint32_t cid) {
    return guarded<uint8_t>(nullptr, 0, [&]() -> uint8_t {
        if (!p || cid >= g_class_count) return 0;
        JNIEnv* env = get_env(); if (!env) return 0;
        if (!g_classes[cid].resolved.load(std::memory_order_acquire)) resolve_class(env, cid);
        return env->IsInstanceOf((jobject)(uintptr_t)p, g_classes[cid].class_ref) != JNI_FALSE;
    });
}
uint8_t stratum_is_same_object(int64_t a, int64_t b) {
    if (!a && !b) return 1; if (!a || !b) return 0;
    JNIEnv* env = get_env();
    return env ? env->IsSameObject((jobject)(uintptr_t)a, (jobject)(uintptr_t)b) != JNI_FALSE : a == b;
}
int64_t stratum_new_string(const StratumStr* s) {
    JNIEnv* env = get_env(); if (!env || !s || !s->ptr) return 0;
    jstring js = stratum_str_to_jstring(env, s->ptr, s->len);
    int64_t g = (int64_t)(uintptr_t)env->NewGlobalRef(js); env->DeleteLocalRef(js); return g;
}
StratumStr stratum_to_string(int64_t p) {
    JNIEnv* env = get_env(); if (!env || !p) return StratumStr{nullptr, 0};
    JniLocalFrame fr(env, 8);
    t_buf = object_to_std_string(env, (jobject)(uintptr_t)p);
    return StratumStr{t_buf.data(), t_buf.size()};
}
int32_t stratum_hash_code(int64_t p) {
    if (!p) return 0; JNIEnv* env = get_env(); if (!env) return 0;
    JniLocalFrame fr(env, 8);
    jclass cls = env->GetObjectClass((jobject)(uintptr_t)p);
    jmethodID mid = env->GetMethodID(cls, "hashCode", "()I");
    jint h = mid ? env->CallIntMethod((jobject)(uintptr_t)p, mid) : 0;
    if (env->ExceptionCheck()) { env->ExceptionClear(); h = 0; }
    return h;
}
int64_t stratum_box(uint8_t kind, int64_t bits) {
    JNIEnv* env = get_env(); if (!env) return 0;
    JniLocalFrame fr(env, 8);
    for (auto& pr : g_prims) {
        if ((uint8_t)pr.kind != kind) continue;
        jvalue jv; jv.j = 0;
        switch (kind) {
            case 'Z': jv.z = bits ? JNI_TRUE : JNI_FALSE; break; case 'B': jv.b = (jbyte)bits; break;
            case 'C': jv.c = (jchar)bits; break;              case 'S': jv.s = (jshort)bits; break;
            case 'I': jv.i = (jint)bits; break;               case 'J': jv.j = (jlong)bits; break;
            case 'F': { uint32_t b = (uint32_t)bits; memcpy(&jv.f, &b, sizeof(b)); break; }
            case 'D': { memcpy(&jv.d, &bits, sizeof(bits)); break; }
        }
        jobject o = env->CallStaticObjectMethodA(pr.c, pr.valueOf, &jv);
        return o ? (int64_t)(uintptr_t)env->NewGlobalRef(o) : 0;
    }
    return 0;
}
int64_t stratum_new_array(uint8_t kind, const void* data, size_t len) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env) return 0;
        if (len > 0x7fffffffu) throw std::runtime_error("Stratum: array too large");
        if (len && !data) throw std::runtime_error("Stratum: null data for non-empty array");
        JniLocalFrame fr(env, 8);
        const jsize n = (jsize)len;
        jarray arr = nullptr;
#define MK_ARR(K, JT, NEWF, SETF, CT) \
        case K: { JT a = env->NEWF(n); if (a && n) env->SETF(a, 0, n, (const CT*)data); arr = a; break; }
        switch (kind) {
            MK_ARR('B', jbyteArray,    NewByteArray,    SetByteArrayRegion,    jbyte)
            MK_ARR('I', jintArray,     NewIntArray,     SetIntArrayRegion,     jint)
            MK_ARR('J', jlongArray,    NewLongArray,    SetLongArrayRegion,    jlong)
            MK_ARR('F', jfloatArray,   NewFloatArray,   SetFloatArrayRegion,   jfloat)
            MK_ARR('D', jdoubleArray,  NewDoubleArray,  SetDoubleArrayRegion,  jdouble)
            MK_ARR('Z', jbooleanArray, NewBooleanArray, SetBooleanArrayRegion, jboolean)
            MK_ARR('C', jcharArray,    NewCharArray,    SetCharArrayRegion,    jchar)
            MK_ARR('S', jshortArray,   NewShortArray,   SetShortArrayRegion,   jshort)
            default: throw std::runtime_error("Stratum: unsupported array kind");
        }
#undef MK_ARR
        if (!arr) { stratum_check_java_exc(env); throw std::runtime_error("Stratum: OOM allocating array"); }
        return (int64_t)(uintptr_t)env->NewGlobalRef(arr);
    });
}

int64_t stratum_new_object_array(const char* elem_class, const int64_t* items, size_t n) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env) return 0;
        if (!elem_class || !*elem_class) throw std::runtime_error("Stratum: new_object_array: empty element class");
        if (n > 0x7fffffffu) throw std::runtime_error("Stratum: array too large");
        if (n && !items) throw std::runtime_error("Stratum: null items for non-empty array");
        JniLocalFrame fr(env, 16);
        std::string e = elem_class;
        for (char& c : e) if (c == '.') c = '/';
        jclass ec = find_class(env, e.c_str());            // also accepts "[I", "[[Lx/Y;"
        if (!ec) throw std::runtime_error(std::string("Stratum: array element class not found: ") + elem_class);
        jobjectArray arr = env->NewObjectArray((jsize)n, ec, nullptr);
        env->DeleteLocalRef(ec);
        if (!arr) { stratum_check_java_exc(env); throw std::runtime_error("Stratum: OOM allocating array"); }
        for (size_t k = 0; k < n; ++k) {
            if (items[k]) {
                env->SetObjectArrayElement(arr, (jsize)k, (jobject)(uintptr_t)items[k]);
                stratum_check_java_exc(env);               // ArrayStoreException -> readable error
            }
        }
        return (int64_t)(uintptr_t)env->NewGlobalRef(arr);
    });
}

int64_t stratum_new_list(const int64_t* items, size_t n) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env) return 0; JniLocalFrame fr(env, 32);
        jclass ac = find_class(env, "java/util/ArrayList");
        jmethodID ctor = env->GetMethodID(ac, "<init>", "()V"), add = env->GetMethodID(ac, "add", "(Ljava/lang/Object;)Z");
        jobject l = env->NewObject(ac, ctor);
        for (size_t k = 0; k < n; ++k) env->CallBooleanMethod(l, add, (jobject)(uintptr_t)items[k]);
        stratum_check_java_exc(env);
        return (int64_t)(uintptr_t)env->NewGlobalRef(l);
    });
}
int64_t stratum_unbox(int64_t obj_handle, uint8_t kind, int32_t* err) {
    return guarded<int64_t>(err, 0, [&]() -> int64_t {
        if (!obj_handle) return 0;
        JNIEnv* env = get_env();
        if (!env) return 0;
        jobject obj = (jobject)(uintptr_t)obj_handle;
        for (auto& pr : g_prims) {
            if ((uint8_t)pr.kind != kind) continue;
            if (!pr.c || !env->IsInstanceOf(obj, pr.c)) {
                throw std::runtime_error("Stratum: object is not an instance of the requested boxed type");
            }
            int64_t res = 0;
            switch (kind) {
                case 'Z': res = env->CallBooleanMethod(obj, pr.unbox) != JNI_FALSE; break;
                case 'B': res = (int64_t)env->CallByteMethod(obj, pr.unbox); break;
                case 'C': res = (int64_t)env->CallCharMethod(obj, pr.unbox); break;
                case 'S': res = (int64_t)env->CallShortMethod(obj, pr.unbox); break;
                case 'I': res = (int64_t)env->CallIntMethod(obj, pr.unbox); break;
                case 'J': res = env->CallLongMethod(obj, pr.unbox); break;
                case 'F': { jfloat f = env->CallFloatMethod(obj, pr.unbox); uint32_t b; memcpy(&b, &f, 4); res = b; break; }
                case 'D': { jdouble d = env->CallDoubleMethod(obj, pr.unbox); memcpy(&res, &d, 8); break; }
            }
            stratum_check_java_exc(env);
            return res;
        }
        throw std::runtime_error("Stratum: unsupported unbox kind");
    });
}

int64_t stratum_new_map(const int64_t* keys, const int64_t* vals, size_t n) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env) return 0; JniLocalFrame fr(env, 32);
        jclass hc = find_class(env, "java/util/HashMap");
        jmethodID ctor = env->GetMethodID(hc, "<init>", "()V");
        jmethodID put = env->GetMethodID(hc, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
        jobject m = env->NewObject(hc, ctor);
        for (size_t k = 0; k < n; ++k) { jobject pv = env->CallObjectMethod(m, put, (jobject)(uintptr_t)keys[k], (jobject)(uintptr_t)vals[k]); if (pv) env->DeleteLocalRef(pv); }
        stratum_check_java_exc(env);
        return (int64_t)(uintptr_t)env->NewGlobalRef(m);
    });
}

void stratum_get_map_entries(int64_t map_handle, StratumArr* keys_out, StratumArr* vals_out, int32_t* err) {
    keys_out->kind = 0; keys_out->len = 0; keys_out->data = nullptr;
    vals_out->kind = 0; vals_out->len = 0; vals_out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int {
        if (!map_handle) return 0;
        JNIEnv* env = get_env();
        if (!env) return 0;
        JniLocalFrame fr(env, 64);
        jobject map = (jobject)(uintptr_t)map_handle;
        jclass map_cls = env->GetObjectClass(map);
        jmethodID m_entrySet = env->GetMethodID(map_cls, "entrySet", "()Ljava/util/Set;");
        env->DeleteLocalRef(map_cls);
        if (!m_entrySet) { env->ExceptionClear(); return 0; }
        jobject es = env->CallObjectMethod(map, m_entrySet);
        if (!es) return 0;
        jclass set_cls = env->GetObjectClass(es);
        jmethodID m_iter = env->GetMethodID(set_cls, "iterator", "()Ljava/util/Iterator;");
        jmethodID m_size = env->GetMethodID(set_cls, "size", "()I");
        env->DeleteLocalRef(set_cls);
        if (!m_iter) { env->ExceptionClear(); return 0; }
        jsize n = m_size ? env->CallIntMethod(es, m_size) : 0;
        jobject iter = env->CallObjectMethod(es, m_iter);
        if (!iter) return 0;
        jclass ic = env->GetObjectClass(iter);
        jmethodID m_hn = env->GetMethodID(ic, "hasNext", "()Z");
        jmethodID m_nx = env->GetMethodID(ic, "next", "()Ljava/lang/Object;");
        env->DeleteLocalRef(ic);
        jclass entry_iface = find_class(env, "java/util/Map$Entry");
        jmethodID m_key = entry_iface ? env->GetMethodID(entry_iface, "getKey", "()Ljava/lang/Object;") : nullptr;
        jmethodID m_val = entry_iface ? env->GetMethodID(entry_iface, "getValue", "()Ljava/lang/Object;") : nullptr;
        if (entry_iface) env->DeleteLocalRef(entry_iface);

        std::vector<int64_t> k_vec, v_vec;
        if (n > 0) { k_vec.reserve(n); v_vec.reserve(n); }
        while (m_hn && m_nx && m_key && m_val && env->CallBooleanMethod(iter, m_hn)) {
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            JniLocalFrame loop_fr(env, 16);
            jobject entry = env->CallObjectMethod(iter, m_nx);
            if (!entry) continue;
            jobject k = env->CallObjectMethod(entry, m_key);
            jobject v = env->CallObjectMethod(entry, m_val);
            k_vec.push_back(k ? (int64_t)(uintptr_t)env->NewGlobalRef(k) : 0);
            v_vec.push_back(v ? (int64_t)(uintptr_t)env->NewGlobalRef(v) : 0);
        }
        stratum_check_java_exc(env);

        size_t count = k_vec.size();
        keys_out->kind = 'O'; keys_out->len = count;
        vals_out->kind = 'O'; vals_out->len = count;
        if (count > 0) {
            int64_t* kd = (int64_t*)malloc(sizeof(int64_t) * count);
            int64_t* vd = (int64_t*)malloc(sizeof(int64_t) * count);
            memcpy(kd, k_vec.data(), sizeof(int64_t) * count);
            memcpy(vd, v_vec.data(), sizeof(int64_t) * count);
            keys_out->data = kd;
            vals_out->data = vd;
        }
        return 0;
    });
}

// ── zero-copy buffers / surfaces (caller keeps the ByteBuffer Object alive while using the pointer) ──
void* stratum_get_direct_buffer_address(int64_t bb) { JNIEnv* env = get_env(); return (env && bb) ? env->GetDirectBufferAddress((jobject)(uintptr_t)bb) : nullptr; }
size_t stratum_get_direct_buffer_capacity(int64_t bb) {
    JNIEnv* env = get_env(); if (!env || !bb) return 0;
    jlong c = env->GetDirectBufferCapacity((jobject)(uintptr_t)bb); return c > 0 ? (size_t)c : 0;
}
void stratum_get_bytebuffer_data(int64_t bb, StratumArr* out, int32_t* err) {
    out->kind = 0; out->len = 0; out->data = nullptr;
    guarded<int>(err, 0, [&]() -> int {
        JNIEnv* env = get_env();
        if (!env || !bb) return 0;
        jobject jbb = (jobject)(uintptr_t)bb;

        // Try direct buffer first
        void* addr = env->GetDirectBufferAddress(jbb);
        jlong cap = addr ? env->GetDirectBufferCapacity(jbb) : 0;
        if (addr && cap > 0) {
            out->kind = 'B';
            out->len = (size_t)cap;
            out->data = malloc((size_t)cap);
            memcpy(out->data, addr, (size_t)cap);
            return 0;
        }

        // Fallback to heap buffer array()
        JniLocalFrame fr(env, 8);
        jclass bcls = env->GetObjectClass(jbb);
        jmethodID mid = env->GetMethodID(bcls, "array", "()[B");
        if (!mid) { env->ExceptionClear(); return 0; }
        jbyteArray ba = (jbyteArray)env->CallObjectMethod(jbb, mid);
        stratum_check_java_exc(env);
        if (ba) fill_arr(env, ba, out);
        return 0;
    });
}
int64_t stratum_allocate_direct_buffer(size_t cap) {
    return guarded<int64_t>(nullptr, 0, [&]() -> int64_t {
        JNIEnv* env = get_env(); if (!env || !cap) return 0; JniLocalFrame fr(env, 8);
        jclass c = find_class(env, "java/nio/ByteBuffer");
        jmethodID m = env->GetStaticMethodID(c, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
        jobject bb = env->CallStaticObjectMethod(c, m, (jint)cap); stratum_check_java_exc(env);
        return bb ? (int64_t)(uintptr_t)env->NewGlobalRef(bb) : 0;
    });
}
void* stratum_surface_to_native_window(int64_t s) { JNIEnv* env = get_env(); return (env && s) ? (void*)ANativeWindow_fromSurface(env, (jobject)(uintptr_t)s) : nullptr; }
void stratum_release_native_window(void* w) { if (w) ANativeWindow_release((ANativeWindow*)w); }

} // extern "C"