// stratum/core.hpp — Object, argument views, callbacks. Hand-written, shared by every generated header.
#pragma once
#include <stratum_c_abi.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stratum {

template <class T> struct dependent_false : std::false_type {};

struct JavaException : std::runtime_error { using std::runtime_error::runtime_error; };
inline void check(int32_t err) { if (err) throw JavaException(stratum_last_error()); }
inline std::string to_std(const StratumStr& s) { return s.ptr ? std::string(s.ptr, s.len) : std::string(); }

// ── Object: owns one JNI global ref. Copy = clone ref. T() is always NULL. ──
class Object {
protected:
    int64_t ptr_ = 0;
public:
    Object() noexcept = default;
    static Object adopt(int64_t h) noexcept { Object o; o.ptr_ = h; return o; }     // takes ownership of a global ref
    ~Object() { if (ptr_) stratum_delete_ref(ptr_); }
    Object(const Object& o) : ptr_(o.ptr_ ? stratum_clone_ref(o.ptr_) : 0) {}
    Object& operator=(const Object& o) { if (this != &o) { if (ptr_) stratum_delete_ref(ptr_); ptr_ = o.ptr_ ? stratum_clone_ref(o.ptr_) : 0; } return *this; }
    Object(Object&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = 0; }
    Object& operator=(Object&& o) noexcept { if (this != &o) { if (ptr_) stratum_delete_ref(ptr_); ptr_ = o.ptr_; o.ptr_ = 0; } return *this; }

    int64_t raw() const noexcept { return ptr_; }
    int64_t release() noexcept { int64_t p = ptr_; ptr_ = 0; return p; }
    bool is_null() const noexcept { return ptr_ == 0; }
    explicit operator bool() const noexcept { return ptr_ != 0; }
    bool operator==(const Object& o) const noexcept { return stratum_is_same_object(ptr_, o.ptr_) != 0; }
    bool operator!=(const Object& o) const noexcept { return !(*this == o); }
    std::string to_string() const { return to_std(stratum_to_string(ptr_)); }
    int32_t hash_code() const noexcept { return stratum_hash_code(ptr_); }
    template <class T> bool is_instance_of() const noexcept { return stratum_is_instance_of(ptr_, T::_CLASS_ID) != 0; }
    template <class T> T as() const {            // checked downcast (JNI IsInstanceOf)
        if constexpr (std::is_same_v<T, Object>) { return *this; }
        else {
            if (!ptr_) return T();
            if (!is_instance_of<T>()) throw std::runtime_error(std::string("Stratum: object is not an instance of ") + T::_FQN);
            return T::_wrap(stratum_clone_ref(ptr_));
        }
    }
    template <class T> T unchecked_as() const {
        if constexpr (std::is_same_v<T, Object>) { return *this; }
        else { return ptr_ ? T::_wrap(stratum_clone_ref(ptr_)) : T(); }
    }

    // Boxed primitive unbox helpers
    bool     as_bool() const   { int32_t e=0; auto r = stratum_unbox(ptr_, 'Z', &e); check(e); return r != 0; }
    int8_t   as_i8() const     { int32_t e=0; auto r = stratum_unbox(ptr_, 'B', &e); check(e); return (int8_t)r; }
    uint16_t as_char() const   { int32_t e=0; auto r = stratum_unbox(ptr_, 'C', &e); check(e); return (uint16_t)r; }
    int16_t  as_i16() const    { int32_t e=0; auto r = stratum_unbox(ptr_, 'S', &e); check(e); return (int16_t)r; }
    int32_t  as_i32() const    { int32_t e=0; auto r = stratum_unbox(ptr_, 'I', &e); check(e); return (int32_t)r; }
    int64_t  as_i64() const    { int32_t e=0; auto r = stratum_unbox(ptr_, 'J', &e); check(e); return r; }
    float    as_float() const  { int32_t e=0; auto r = stratum_unbox(ptr_, 'F', &e); check(e); uint32_t b = (uint32_t)r; float f; std::memcpy(&f, &b, 4); return f; }
    double   as_double() const { int32_t e=0; auto r = stratum_unbox(ptr_, 'D', &e); check(e); double d; std::memcpy(&d, &r, 8); return d; }

    template <class T> std::vector<T> as_array() const;
    std::vector<std::pair<Object, Object>> as_map() const;
};

// ── boxing / strings / collections ──
inline Object box(char kind, int64_t bits) { return Object::adopt(stratum_box((uint8_t)kind, bits)); }
inline Object box_bool(bool v) { return box('Z', v); }
inline Object box_i8(int8_t v) { return box('B', v); }
inline Object box_i16(int16_t v) { return box('S', v); }
inline Object box_char(uint16_t v) { return box('C', v); }
inline Object box_i32(int32_t v) { return box('I', v); }
inline Object box_i64(int64_t v) { return box('J', v); }
inline Object box_f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); return box('F', b); }
inline Object box_f64(double v) { int64_t b; std::memcpy(&b, &v, 8); return box('D', b); }
inline Object java_string(std::string_view s) { StratumStr x{s.data(), s.size()}; return Object::adopt(stratum_new_string(&x)); }
inline Object new_list(const std::vector<Object>& v) {
    std::vector<int64_t> h; for (auto& o : v) h.push_back(o.raw());
    return Object::adopt(stratum_new_list(h.data(), h.size()));
}
inline Object new_map(const std::vector<std::pair<Object, Object>>& v) {
    std::vector<int64_t> k, x; for (auto& p : v) { k.push_back(p.first.raw()); x.push_back(p.second.raw()); }
    return Object::adopt(stratum_new_map(k.data(), x.data(), k.size()));
}

// ── Java array creation ─────────────────────────────────────────────────────
// elem_class: "java/lang/String", "android.view.Surface", or an array descriptor ("[I" => int[][], "[[I" => int[][][]).
inline Object new_object_array(std::string_view elem_class, const std::vector<Object>& items) {
    std::string e(elem_class);
    std::vector<int64_t> h; h.reserve(items.size());
    for (auto& o : items) h.push_back(o.raw());
    int64_t r = stratum_new_object_array(e.c_str(), h.data(), h.size());
    if (!r) throw JavaException(stratum_last_error());
    return Object::adopt(r);
}

namespace arr_detail {
template <class T> struct is_container : std::false_type {};
template <class T, class A> struct is_container<std::vector<T, A>> : std::true_type {};
template <class T> struct is_container<std::initializer_list<T>> : std::true_type {};
template <class T> struct depth { static constexpr size_t value = 0; };
template <class T, class A> struct depth<std::vector<T, A>> { static constexpr size_t value = 1 + depth<T>::value; };
template <class T> struct depth<std::initializer_list<T>> { static constexpr size_t value = 1 + depth<T>::value; };
template <class T> struct leaf { using type = T; };
template <class T, class A> struct leaf<std::vector<T, A>> { using type = typename leaf<T>::type; };
template <class T> struct leaf<std::initializer_list<T>> { using type = typename leaf<T>::type; };

template <class T> inline constexpr bool is_str_v = std::is_convertible_v<const T&, std::string_view>;
template <class T> inline constexpr bool is_obj_v = std::is_base_of_v<Object, T>;

// C++ element type -> Java primitive kind. sizeof-based, so int / long long / int64_t all behave on 32- and 64-bit.
template <class T> constexpr char prim_kind() {
    using D = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<D, bool>) return 'Z';
    else if constexpr (std::is_integral_v<D>) {
        if constexpr (sizeof(D) == 1) return 'B';
        else if constexpr (sizeof(D) == 2) return std::is_unsigned_v<D> ? 'C' : 'S';
        else if constexpr (sizeof(D) == 4) return 'I';
        else if constexpr (sizeof(D) == 8) return 'J';
        else { static_assert(dependent_false<D>::value, "new_array: unsupported integer size"); return 0; }
    }
    else if constexpr (std::is_same_v<D, float>) return 'F';
    else if constexpr (std::is_same_v<D, double>) return 'D';
    else { static_assert(dependent_false<D>::value, "new_array: unsupported element type"); return 0; }
}
template <class L> std::string leaf_name() {
    if constexpr (is_str_v<L>) return "java/lang/String";
    else if constexpr (std::is_same_v<L, Object>) return "java/lang/Object";
    else if constexpr (is_obj_v<L>) return L::_FQN;
    else return std::string(1, prim_kind<L>());
}
// Element class of a `dims`-dimensional array whose innermost element type is L.
template <class L> std::string elem_class(size_t dims) {
    if (dims <= 1) return leaf_name<L>();
    constexpr bool ref = is_str_v<L> || is_obj_v<L>;
    return std::string(dims - 1, '[') + (ref ? "L" + leaf_name<L>() + ";" : leaf_name<L>());
}
template <class S> Object to_jstr(const S& s) {
    if constexpr (std::is_pointer_v<S>) return s ? java_string(std::string_view(s)) : Object();
    else return java_string(std::string_view(s));
}

template <class C> Object build(const C& c) {           // C = std::vector<E> or std::initializer_list<E>
    using E = typename C::value_type;
    if constexpr (is_container<E>::value) {
        using L = typename leaf<E>::type;
        std::vector<Object> rows; rows.reserve(c.size());
        for (const auto& item : c) rows.push_back(build(item));
        return new_object_array(elem_class<L>(1 + depth<E>::value), rows);
    } else if constexpr (is_str_v<E>) {
        std::vector<Object> o; o.reserve(c.size());
        for (const auto& s : c) o.push_back(to_jstr(s));
        return new_object_array("java/lang/String", o);
    } else if constexpr (is_obj_v<E>) {
        std::vector<Object> o(c.begin(), c.end());
        return new_object_array(leaf_name<E>(), o);
    } else if constexpr (std::is_same_v<E, bool>) {
        std::vector<uint8_t> t(c.begin(), c.end());
        int64_t h = stratum_new_array('Z', t.data(), t.size());
        if (!h) throw JavaException(stratum_last_error());
        return Object::adopt(h);
    } else {
        int64_t h = stratum_new_array((uint8_t)prim_kind<E>(), std::data(c), std::size(c));
        if (!h) throw JavaException(stratum_last_error());
        return Object::adopt(h);
    }
}
} // namespace arr_detail

// One name for everything: new_array(...) -> Java array Object of the right type.
template <class T> Object new_array(const T* p, size_t n) {
    int64_t h = stratum_new_array((uint8_t)arr_detail::prim_kind<T>(), p, n);
    if (!h) throw JavaException(stratum_last_error());
    return Object::adopt(h);
}
template <class T> Object new_array(const std::vector<T>& v) { return arr_detail::build(v); }            // any depth
template <class T> Object new_array(std::initializer_list<T> l) { return arr_detail::build(l); }          // {1,2,3}
template <class T> Object new_array(std::initializer_list<std::initializer_list<T>> l) { return arr_detail::build(l); }   // {{1,2},{3,4}}
template <class T> Object new_array(std::initializer_list<std::initializer_list<std::initializer_list<T>>> l) { return arr_detail::build(l); }
template <class T> Object new_array(std::initializer_list<std::initializer_list<std::initializer_list<std::initializer_list<T>>>> l) { return arr_detail::build(l); }
inline Object new_string_array(const std::vector<std::string>& v) { return arr_detail::build(v); }

// ── argument converters ──
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
inline StratumArg to_arg(T v) noexcept { StratumArg a; a.i = (int64_t)v; return a; }
inline StratumArg to_arg(float v) noexcept { StratumArg a; a.i = 0; a.f = v; return a; }
inline StratumArg to_arg(double v) noexcept { StratumArg a; a.d = v; return a; }
inline StratumArg to_arg(const Object& o) noexcept { StratumArg a; a.i = o.raw(); return a; }
// bool that refuses "string literal"->bool conversion (keeps put(String,String) from picking put(String,Boolean))
struct Bool { bool v; template <class B, std::enable_if_t<std::is_same_v<B, bool>, int> = 0> Bool(B b) : v(b) {} };
inline StratumArg to_arg(Bool b) noexcept { StratumArg a; a.i = b.v ? 1 : 0; return a; }

class JStr {                                  // non-owning UTF-8 view; ptr==NULL => Java null
    StratumStr s_{nullptr, 0};
public:
    JStr(std::nullptr_t) {}
    JStr(const char* p) { if (p) { s_.ptr = p; s_.len = std::strlen(p); } }
    JStr(const std::string& s) : s_{s.data(), s.size()} {}
    JStr(std::string_view v) : s_{v.data() ? v.data() : "", v.size()} {}
    StratumArg arg() const noexcept { StratumArg a; a.p = &s_; return a; }
};
template <class T> class Arr {                // primitive array view; nullptr => Java null; {} => empty array
    StratumSlice s_{nullptr, 0}; bool null_ = false;
public:
    Arr(std::nullptr_t) : null_(true) {}
    Arr(const std::vector<T>& v) : s_{v.data(), v.size()} {}
    Arr(std::initializer_list<T> l) : s_{l.begin(), l.size()} {}
    Arr(const T* p, size_t n) : s_{p, n} {}
    StratumArg arg() const noexcept { StratumArg a; a.p = null_ ? nullptr : &s_; return a; }
};
class StrArr {                                // String[]
    std::vector<StratumStr> it_; StratumSlice s_{nullptr, 0}; bool null_ = false;
    void fin() { s_.ptr = it_.data(); s_.len = it_.size(); }
public:
    StrArr(std::nullptr_t) : null_(true) {}
    StrArr(const std::vector<std::string>& v) { for (auto& x : v) it_.push_back({x.data(), x.size()}); fin(); }
    StrArr(std::initializer_list<const char*> l) { for (auto p : l) it_.push_back({p, p ? std::strlen(p) : 0}); fin(); }
    StrArr(const StrArr&) = delete; StrArr& operator=(const StrArr&) = delete;
    StratumArg arg() const noexcept { StratumArg a; a.p = null_ ? nullptr : &s_; return a; }
};
class ObjArr {                                // Object[] / View[] ...
    std::vector<int64_t> h_; StratumSlice s_{nullptr, 0}; bool null_ = false;
    void fin() { s_.ptr = h_.data(); s_.len = h_.size(); }
public:
    ObjArr(std::nullptr_t) : null_(true) {}
    ObjArr(const std::vector<Object>& v) { for (auto& o : v) h_.push_back(o.raw()); fin(); }
    ObjArr(std::initializer_list<Object> l) { for (auto& o : l) h_.push_back(o.raw()); fin(); }
    ObjArr(const ObjArr&) = delete; ObjArr& operator=(const ObjArr&) = delete;
    StratumArg arg() const noexcept { StratumArg a; a.p = null_ ? nullptr : &s_; return a; }
};
class ListArg {                               // java.util.List / Collection / Iterable
    std::vector<int64_t> h_; StratumListArg r_{0, nullptr, 0}; bool null_ = false;
public:
    ListArg(std::nullptr_t) : null_(true) {}
    ListArg(const Object& existing) { r_.existing = existing.raw(); }
    ListArg(const std::vector<Object>& v) { for (auto& o : v) h_.push_back(o.raw()); r_.ptrs = h_.data(); r_.len = h_.size(); }
    ListArg(std::initializer_list<Object> l) { for (auto& o : l) h_.push_back(o.raw()); r_.ptrs = h_.data(); r_.len = h_.size(); }
    ListArg(const ListArg&) = delete; ListArg& operator=(const ListArg&) = delete;
    StratumArg arg() const noexcept { StratumArg a; a.p = null_ ? nullptr : &r_; return a; }
};
class MapArg {                                // java.util.Map / HashMap
    std::vector<int64_t> k_, v_; StratumMapArg r_{0, nullptr, nullptr, 0}; bool null_ = false;
public:
    MapArg(std::nullptr_t) : null_(true) {}
    MapArg(const Object& existing) { r_.existing = existing.raw(); }
    MapArg(const std::vector<std::pair<Object, Object>>& v) {
        for (auto& p : v) { k_.push_back(p.first.raw()); v_.push_back(p.second.raw()); }
        r_.keys = k_.data(); r_.vals = v_.data(); r_.len = k_.size();
    }
    MapArg(const MapArg&) = delete; MapArg& operator=(const MapArg&) = delete;
    StratumArg arg() const noexcept { StratumArg a; a.p = null_ ? nullptr : &r_; return a; }
};

// ── returned arrays ──
template <class T> std::vector<T> arr_vec(StratumArr& a) {
    std::vector<T> v; if (a.len && a.data) v.assign((const T*)a.data, (const T*)a.data + a.len);
    stratum_arr_free(&a); return v;
}
inline std::vector<bool> arr_bool(StratumArr& a) { auto b = arr_vec<uint8_t>(a); return std::vector<bool>(b.begin(), b.end()); }
inline std::vector<std::string> arr_strings(StratumArr& a) {
    std::vector<std::string> v;
    if (a.kind == 'T' && a.data) for (size_t i = 0; i < a.len; ++i) v.push_back(to_std(((StratumStr*)a.data)[i]));
    stratum_arr_free(&a); return v;
}
inline std::vector<Object> arr_objs(StratumArr& a) {         // adopts handles; String[] returned for an Object[] decl is converted
    std::vector<Object> v;
    if (a.kind == 'O' && a.data) {
        int64_t* h = (int64_t*)a.data;
        for (size_t i = 0; i < a.len; ++i) v.push_back(Object::adopt(h[i]));
        free(a.data); a.data = nullptr; a.len = 0;
    } else if (a.kind == 'T' && a.data) {
        StratumStr* it = (StratumStr*)a.data;
        for (size_t i = 0; i < a.len; ++i)
            v.push_back(it[i].ptr ? java_string(std::string_view(it[i].ptr, it[i].len)) : Object());
        stratum_arr_free(&a);
    } else if (a.data) {
        stratum_arr_free(&a);
    }
    return v;
}

inline std::vector<std::pair<Object, Object>> map_entries(const Object& map_obj) {
    StratumArr k_out{0, 0, nullptr}, v_out{0, 0, nullptr};
    int32_t err = 0;
    stratum_get_map_entries(map_obj.raw(), &k_out, &v_out, &err);
    check(err);
    auto k_objs = arr_objs(k_out);
    auto v_objs = arr_objs(v_out);
    std::vector<std::pair<Object, Object>> res;
    size_t count = k_objs.size();
    res.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        res.emplace_back(std::move(k_objs[i]), std::move(v_objs[i]));
    }
    return res;
}

template <class T> inline std::vector<T> Object::as_array() const {
    StratumArr out{0, 0, nullptr};
    int32_t err = 0;
    stratum_get_array_data(ptr_, &out, &err);
    check(err);
    if constexpr (std::is_same_v<T, bool>) {
        return arr_bool(out);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return arr_strings(out);
    } else if constexpr (std::is_same_v<T, Object>) {
        return arr_objs(out);
    } else {
        return arr_vec<T>(out);
    }
}

inline std::vector<std::pair<Object, Object>> Object::as_map() const {
    return map_entries(*this);
}

// ── callbacks ──
class CbArg {                                 // one argument of a Java->C++ callback; converts to what the lambda asks for
    const StratumVal* v_;
public:
    explicit CbArg(const StratumVal* v = nullptr) : v_(v) {}
    bool is_null() const { return !v_ || v_->tag == STRATUM_V_NULL; }
    Object obj() const { return (v_ && v_->tag == STRATUM_V_OBJ) ? Object::adopt(stratum_clone_ref(v_->v.i)) : Object(); }   // clone: safe to keep
    int64_t i() const { return !v_ ? 0 : (v_->tag == STRATUM_V_FLOAT || v_->tag == STRATUM_V_DOUBLE) ? (int64_t)v_->v.d : v_->v.i; }
    double d() const { return !v_ ? 0 : (v_->tag == STRATUM_V_FLOAT || v_->tag == STRATUM_V_DOUBLE) ? v_->v.d : (double)v_->v.i; }
    bool b() const { return i() != 0; }
    std::string str() const {
        if (!v_) return {};
        if (v_->tag == STRATUM_V_STR) return std::string((const char*)v_->v.s.ptr, v_->v.s.len);
        if (v_->tag == STRATUM_V_OBJ) return Object::adopt(stratum_clone_ref(v_->v.i)).to_string();
        return {};
    }
    template <class T> T as() const { return obj().template as<T>(); }
    template <class T> operator T() const {
        if constexpr (std::is_same_v<T, bool>) return b();
        else if constexpr (std::is_integral_v<T>) return (T)i();
        else if constexpr (std::is_floating_point_v<T>) return (T)d();
        else if constexpr (std::is_same_v<T, std::string>) return str();
        else if constexpr (std::is_same_v<T, Object>) return obj();
        else { static_assert(std::is_base_of_v<Object, T>, "unsupported callback argument type"); return obj().template as<T>(); }
    }
};
class Args {                                  // optional raw form: [](stratum::Args a){ a[0].as<View>(); a.size(); }
    const StratumVal* a_; size_t n_;
public:
    Args(const StratumVal* a, size_t n) : a_(a), n_(n) {}
    size_t size() const { return n_; }
    CbArg operator[](size_t i) const { return CbArg(i < n_ ? a_ + i : nullptr); }
};

inline std::string& ret_buf() { static thread_local std::string s; return s; }
inline std::vector<int32_t>& ret_ints() { static thread_local std::vector<int32_t> v; return v; }
inline void set_err(StratumVal* o, const char* m) { ret_buf() = m; o->tag = STRATUM_V_ERR; o->v.s.ptr = ret_buf().data(); o->v.s.len = ret_buf().size(); }

template <class R> void set_ret(StratumVal* out, R&& r) {
    using D = std::decay_t<R>;
    if constexpr (std::is_same_v<D, bool>) { out->tag = STRATUM_V_BOOL; out->v.i = r; }
    else if constexpr (std::is_integral_v<D>) { out->tag = sizeof(D) <= 4 ? STRATUM_V_INT : STRATUM_V_LONG; out->v.i = (int64_t)r; }
    else if constexpr (std::is_same_v<D, float>) { out->tag = STRATUM_V_FLOAT; out->v.d = r; }
    else if constexpr (std::is_same_v<D, double>) { out->tag = STRATUM_V_DOUBLE; out->v.d = r; }
    else if constexpr (std::is_convertible_v<D, std::string_view>) {
        ret_buf() = std::string(std::string_view(r)); out->tag = STRATUM_V_STR; out->v.s.ptr = ret_buf().data(); out->v.s.len = ret_buf().size();
    }
    else if constexpr (std::is_base_of_v<Object, D>) {
        if (r) { out->tag = STRATUM_V_OBJ; out->v.i = stratum_clone_ref(r.raw()); } else out->tag = STRATUM_V_NULL;
    }
    else if constexpr (std::is_same_v<D, std::vector<int32_t>>) {
        ret_ints() = r; out->tag = STRATUM_V_I32ARR; out->v.s.ptr = ret_ints().data(); out->v.s.len = ret_ints().size();
    }
    else static_assert(dependent_false<D>::value, "unsupported callback return type");
}

template <size_t> struct CbArgS { using type = CbArg; };
template <class F, size_t... I> constexpr bool invocable_n(std::index_sequence<I...>) { return std::is_invocable_v<F&, typename CbArgS<I>::type...>; }
template <class F, size_t N = 0> constexpr int cb_arity() {
    if constexpr (invocable_n<F>(std::make_index_sequence<N>{})) return (int)N;
    else if constexpr (N < 8) return cb_arity<F, N + 1>();
    else return -1;
}
template <class F, size_t... I> decltype(auto) call_n(F& f, const StratumVal* a, size_t n, std::index_sequence<I...>) {
    (void)a; (void)n; return f(CbArg(I < n ? a + I : nullptr)...);
}
template <class F> void invoke_cb(F& f, const StratumVal* a, size_t n, StratumVal* out) {
    constexpr int N = cb_arity<F>();
    if constexpr (N >= 0) {
        using Seq = std::make_index_sequence<N>;
        using R = decltype(call_n(f, a, n, Seq{}));
        if constexpr (std::is_void_v<R>) call_n(f, a, n, Seq{}); else set_ret(out, call_n(f, a, n, Seq{}));
    } else if constexpr (std::is_invocable_v<F&, Args>) {
        using R = decltype(f(Args(a, n)));
        if constexpr (std::is_void_v<R>) f(Args(a, n)); else set_ret(out, f(Args(a, n)));
    } else static_assert(dependent_false<F>::value, "callback must be callable as f(), f(arg)... (up to 8) or f(stratum::Args)");
}

// Multi-method listeners / abstract classes (WebViewClient, TextWatcher...): Handlers().on("onPageFinished", [](...){})
class Handlers {
public:
    using Fn = std::function<void(const StratumVal*, size_t, StratumVal*)>;
    std::unordered_map<std::string, Fn> map;
    template <class F> Handlers& on(const std::string& name, F f) {
        map[name] = [f](const StratumVal* a, size_t n, StratumVal* o) mutable { invoke_cb(f, a, n, o); };
        return *this;
    }
};

class CbHolder {
    StratumCbDesc d_{nullptr, nullptr, nullptr, 0};
public:
    explicit CbHolder(StratumCbDesc d) : d_(d) {}
    CbHolder(const CbHolder&) = delete; CbHolder& operator=(const CbHolder&) = delete;
    ~CbHolder() { if (d_.userdata && d_.drop) d_.drop(d_.userdata); }       // only if the engine did not take it
    StratumArg arg() const noexcept { StratumArg a; a.p = &d_; return a; }
    const StratumCbDesc* desc() const noexcept { return &d_; }
};
template <class F> CbHolder make_callback(F&& f) {
    using D = std::decay_t<F>;
    StratumCbDesc d{nullptr, nullptr, nullptr, 0};
    if constexpr (std::is_base_of_v<Object, D>) { d.existing = f.raw(); }          // an existing Java listener object
    else if constexpr (std::is_same_v<D, Handlers>) {
        d.userdata = new D(std::forward<F>(f));
        d.fn = [](void* ud, const char* m, const StratumVal* a, size_t n, StratumVal* o) {
            try { auto& h = *static_cast<D*>(ud); auto it = h.map.find(m ? m : ""); if (it != h.map.end()) it->second(a, n, o); }
            catch (const std::exception& e) { set_err(o, e.what()); } catch (...) { set_err(o, "unknown C++ exception in callback"); }
        };
        d.drop = [](void* ud) { delete static_cast<D*>(ud); };
    } else {
        d.userdata = new D(std::forward<F>(f));
        d.fn = [](void* ud, const char*, const StratumVal* a, size_t n, StratumVal* o) {
            try { invoke_cb(*static_cast<D*>(ud), a, n, o); }
            catch (const std::exception& e) { set_err(o, e.what()); } catch (...) { set_err(o, "unknown C++ exception in callback"); }
        };
        d.drop = [](void* ud) { delete static_cast<D*>(ud); };
    }
    return CbHolder(d);
}

} // namespace stratum