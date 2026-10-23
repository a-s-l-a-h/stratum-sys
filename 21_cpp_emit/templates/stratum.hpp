// stratum/stratum.hpp — app-level API: lifecycle auto-wiring, activity, UI thread, callbacks, buffers.
#pragma once
#include <stratum/core.hpp>
#include <stratum/meta_hash.hpp>
#include <stratum/android/app/Activity.hpp>
#include <android/log.h>
#include <cstdlib>
#include <atomic>

namespace stratum {
using Activity = ::stratum::android::app::Activity;

inline Activity getActivity() { return Activity::_wrap(stratum_get_activity()); }
inline void setContentView(const Object& view) { Activity a = getActivity(); stratum_set_content_view(a.raw(), view.raw()); }  // UI thread
inline void setContentView(const Object& act, const Object& view) { stratum_set_content_view(act.raw(), view.raw()); }

template <class F> void register_callback(const std::string& key, F&& f) {      // "StratumService#onCreate", "StratumReceiver#onReceive", ...
    auto h = make_callback(std::forward<F>(f)); stratum_register_callback(key.c_str(), h.desc());
}
template <class F> void register_function(const std::string& name, F&& f) { register_callback("app#" + name, std::forward<F>(f)); } // StratumRuntimeLookup.callPython(name,...)
inline void unregister_prefix(const std::string& p) { stratum_remove_callbacks_by_prefix(p.c_str()); }
template <class F> void run_on_ui_thread(F&& f) { auto h = make_callback(std::forward<F>(f)); check(stratum_post_ui(h.desc())); }
inline Object create_view(const std::string& key) {            // StratumView; register "<key>#onDraw" etc. with register_callback
    int64_t p = stratum_create_view(key.c_str()); if (!p) throw JavaException(stratum_last_error()); return Object::adopt(p);
}
inline Object allocate_direct_buffer(size_t n) { int64_t p = stratum_allocate_direct_buffer(n); if (!p) throw JavaException(stratum_last_error()); return Object::adopt(p); }
inline void* direct_buffer_address(const Object& b) { return stratum_get_direct_buffer_address(b.raw()); }   // keep `b` alive while using it
inline size_t direct_buffer_capacity(const Object& b) { return stratum_get_direct_buffer_capacity(b.raw()); }
inline std::vector<uint8_t> bytebuffer_data(const Object& b) {
    StratumArr out{0, 0, nullptr};
    int32_t err = 0;
    stratum_get_bytebuffer_data(b.raw(), &out, &err);
    check(err);
    return arr_vec<uint8_t>(out);
}
inline void* surface_to_native_window(const Object& s) { return stratum_surface_to_native_window(s.raw()); }
inline void release_native_window(void* w) { stratum_release_native_window(w); }
inline void set_log_enabled(bool e) { stratum_set_log_enabled(e); }

// High-level bridge to com.stratum.runtime.StratumReceiver & StratumService
inline void on_broadcast(std::function<void(Object /*Context*/, Object /*Intent*/)> fn) {
    register_callback("StratumReceiver#onReceive", [fn](Object ctx, Object intent) {
        fn(ctx, intent);
    });
}
inline void on_service_command(std::function<int32_t(Object /*Intent*/, int32_t /*flags*/, int32_t /*startId*/)> fn) {
    register_callback("StratumService#onStartCommand", [fn](Object intent, int32_t flags, int32_t startId) -> int32_t {
        return fn(intent, flags, startId);
    });
}

// Universal custom UI drawing view (matches Python's stratum.ui.CustomCanvasView)
// Direct inheritance from Object avoids circular dependencies when View.hpp isn't generated.
class CustomCanvasView : public Object {
    std::string key_;
public:
    CustomCanvasView(const CustomCanvasView&) = delete;
    CustomCanvasView& operator=(const CustomCanvasView&) = delete;

    CustomCanvasView() {
        static std::atomic<uint64_t> s_vid{0};
        key_ = "view_" + std::to_string(++s_vid);

        register_callback(key_ + "#onDraw", [this](Object canvas) {
            on_draw(canvas);
        });
        register_callback(key_ + "#onMeasure", [this](int32_t w, int32_t h) -> std::vector<int32_t> {
            return on_measure(w, h);
        });
        register_callback(key_ + "#onLayout", [this](bool changed, int32_t l, int32_t t, int32_t r, int32_t b) {
            on_layout(changed, l, t, r, b);
        });
        register_callback(key_ + "#onTouchEvent", [this](Object event) -> bool {
            return on_touch_event(event);
        });

        ptr_ = stratum_create_view(key_.c_str());
        if (!ptr_) throw JavaException(stratum_last_error());
    }

    virtual ~CustomCanvasView() {
        if (!key_.empty()) {
            unregister_prefix(key_ + "#");
        }
    }

    virtual void on_draw(Object canvas) {}
    virtual std::vector<int32_t> on_measure(int32_t width_spec, int32_t height_spec) { return {}; }
    virtual void on_layout(bool changed, int32_t left, int32_t top, int32_t right, int32_t bottom) {}
    virtual bool on_touch_event(Object motion_event) { return false; }
};

namespace internal {
struct Hooks { std::function<void(Activity)> create; std::function<void()> resume, pause, stop, destroy; };
inline Hooks& hooks() { static Hooks h; return h; }
}
inline void on_create(std::function<void(Activity)> f) { internal::hooks().create = std::move(f); }
inline void on_resume(std::function<void()> f)  { internal::hooks().resume = std::move(f); }
inline void on_pause(std::function<void()> f)   { internal::hooks().pause = std::move(f); }
inline void on_stop(std::function<void()> f)    { internal::hooks().stop = std::move(f); }
inline void on_destroy(std::function<void()> f) { internal::hooks().destroy = std::move(f); }
inline void on_back_pressed(std::function<bool()> f) { register_function("onBackPressed", [f]() -> bool { return f(); }); }
} // namespace stratum

// Weak symbols: just define these in the GLOBAL namespace of your lib; no macro needed.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Waddress"
[[gnu::weak]] void onCreate(::stratum::Activity act);
[[gnu::weak]] void onCreate();
[[gnu::weak]] void onResume();
[[gnu::weak]] void onPause();
[[gnu::weak]] void onStop();
[[gnu::weak]] void onDestroy();
[[gnu::weak]] bool onBackPressed();

namespace stratum::internal {
inline int32_t dispatch(const char* ev, int64_t act) {
    try {
        Hooks& h = hooks();
        if (!std::strcmp(ev, "onCreate")) {
            Activity a = Activity::_wrap(act ? stratum_clone_ref(act) : 0);
            if (h.create) h.create(a);
            else { void (*f1)(Activity) = &::onCreate; void (*f0)() = &::onCreate; if (f1) f1(a); else if (f0) f0(); }
        } else {
            std::function<void()>* ex = !std::strcmp(ev, "onResume") ? &h.resume : !std::strcmp(ev, "onPause") ? &h.pause
                                      : !std::strcmp(ev, "onStop") ? &h.stop : !std::strcmp(ev, "onDestroy") ? &h.destroy : nullptr;
            if (ex && *ex) (*ex)();
            else if (ex) {
                void (*f)() = !std::strcmp(ev, "onResume") ? &::onResume : !std::strcmp(ev, "onPause") ? &::onPause
                            : !std::strcmp(ev, "onStop") ? &::onStop : &::onDestroy;
                if (f) f();
            }
        }
    } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR, "Stratum", "%s handler threw: %s", ev, e.what()); }
      catch (...) { __android_log_print(ANDROID_LOG_ERROR, "Stratum", "%s handler threw", ev); }
    return 0;
}
inline struct AutoWire {
    AutoWire() {
#ifndef STRATUM_SKIP_HASH_CHECK
        if (stratum_abi_version() != STRATUM_ABI_VERSION || std::strcmp(stratum_meta_hash(), STRATUM_META_HASH) != 0) {
            __android_log_print(ANDROID_LOG_FATAL, "Stratum", "libstratum.so (%s) and SDK headers (%s) were generated from different inputs. Rebuild both.",
                                stratum_meta_hash(), STRATUM_META_HASH);
            std::abort();
        }
#endif
        stratum_set_lifecycle_dispatcher(&dispatch);
        bool (*back)() = &::onBackPressed;
        if (back) register_function("onBackPressed", [back]() -> bool { return back(); });
    }
} s_auto_wire;
} // namespace stratum::internal
#pragma clang diagnostic pop