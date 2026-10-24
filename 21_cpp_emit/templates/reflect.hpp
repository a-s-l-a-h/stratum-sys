// stratum/reflect.hpp — call ANY Java class by name (your own app classes, third-party SDKs).
#pragma once
#include <stratum/core.hpp>

namespace stratum::reflect {
namespace detail {
template <class D> struct is_vec : std::false_type {};
template <class E, class A> struct is_vec<std::vector<E, A>> : std::true_type {};

struct Pack {
    std::vector<StratumVal> v;
    std::vector<Object> keep;                    // owns temporary arrays until the reflect call returns
    template <class T> void push(const T& x) {
        if constexpr (is_vec<std::decay_t<T>>::value) { keep.push_back(::stratum::new_array(x)); push_one(keep.back()); }
        else push_one(x);
    }
    template <class T> void push_one(const T& x) {
        StratumVal s; std::memset(&s, 0, sizeof(s));
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, bool>) { s.tag = STRATUM_V_BOOL; s.v.i = x; }
        else if constexpr (std::is_same_v<D, int8_t>) { s.tag = STRATUM_V_BYTE; s.v.i = x; }
        else if constexpr (std::is_same_v<D, int16_t>) { s.tag = STRATUM_V_SHORT; s.v.i = x; }
        else if constexpr (std::is_same_v<D, char16_t> || std::is_same_v<D, uint16_t>) { s.tag = STRATUM_V_CHAR; s.v.i = x; }
        else if constexpr (std::is_integral_v<D>) { s.tag = sizeof(D) <= 4 ? STRATUM_V_INT : STRATUM_V_LONG; s.v.i = (int64_t)x; }
        else if constexpr (std::is_same_v<D, float>) { s.tag = STRATUM_V_FLOAT; s.v.d = x; }
        else if constexpr (std::is_same_v<D, double>) { s.tag = STRATUM_V_DOUBLE; s.v.d = x; }
        else if constexpr (std::is_same_v<D, std::nullptr_t>) { s.tag = STRATUM_V_NULL; }
        else if constexpr (std::is_base_of_v<Object, D>) { s.tag = x ? STRATUM_V_OBJ : STRATUM_V_NULL; s.v.i = x.raw(); }
        else if constexpr (std::is_convertible_v<D, std::string_view>) { std::string_view sv(x); s.tag = STRATUM_V_STR; s.v.s.ptr = sv.data(); s.v.s.len = sv.size(); }
        else static_assert(dependent_false<D>::value, "unsupported reflect argument type");
        v.push_back(s);
    }
    template <class... A> explicit Pack(const A&... a) { (push(a), ...); }
};
template <class R> R result(StratumVal& o) {
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_same_v<R, bool>) return o.v.i != 0;
    else if constexpr (std::is_integral_v<R>) return (R)((o.tag == STRATUM_V_FLOAT || o.tag == STRATUM_V_DOUBLE) ? (int64_t)o.v.d : o.v.i);
    else if constexpr (std::is_floating_point_v<R>) return (R)((o.tag == STRATUM_V_FLOAT || o.tag == STRATUM_V_DOUBLE) ? o.v.d : (double)o.v.i);
    else if constexpr (std::is_same_v<R, std::string>) return o.tag == STRATUM_V_STR ? std::string((const char*)o.v.s.ptr, o.v.s.len) : std::string();
    else if constexpr (std::is_base_of_v<Object, R>) { Object x = o.tag == STRATUM_V_OBJ ? Object::adopt(o.v.i) : Object(); return x.template unchecked_as<R>(); }
    else static_assert(dependent_false<R>::value, "unsupported reflect result type");
}
}

template <class R = void, class... A> R call_static(const std::string& cls, const std::string& method, const A&... a) {
    detail::Pack p(a...); StratumVal out{};
    check(stratum_reflect_call(cls.c_str(), 0, method.c_str(), p.v.data(), p.v.size(), &out));
    return detail::result<R>(out);
}
template <class R = void, class... A> R call_method(const Object& target, const std::string& method, const A&... a) {
    if (!target) throw std::runtime_error("reflect: null target");
    detail::Pack p(a...); StratumVal out{};
    check(stratum_reflect_call(nullptr, target.raw(), method.c_str(), p.v.data(), p.v.size(), &out));
    return detail::result<R>(out);
}
template <class... A> Object new_instance(const std::string& cls, const A&... a) {
    detail::Pack p(a...); StratumVal out{};
    check(stratum_reflect_call(cls.c_str(), 0, nullptr, p.v.data(), p.v.size(), &out));
    return detail::result<Object>(out);
}
}