#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 21: thin C++ SDK headers from 05_resolve/output_patched/.
Each header depends ONLY on <stratum/core.hpp> (no cross includes). Inherited methods are flattened into every class,
called with the DECLARING class's class_id. Object returns are untyped `Object` (use .as<T>()).
"""
import argparse, hashlib, json, re, shutil
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENGINE_INC = HERE.parent / "20_engine_core" / "include"
SKIP = {"parse_summary.json", "resolve_summary.json", "manifest.json", "sanitize_summary.json"}
CPP_KW = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl
concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double
dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not
not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed sizeof static
static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using
virtual void volatile wchar_t while xor xor_eq""".split())
MACROS = {"NULL", "EOF", "TRUE", "FALSE", "NAN", "INFINITY", "stdin", "stdout", "stderr", "errno", "unix", "linux", "major", "minor", "min", "max", "assert"}
RESERVED = {"raw", "release", "as", "unchecked_as", "is_null", "to_string", "hash_code", "is_instance_of", "adopt", "create"}

def meta_hash(d: Path) -> str:        # identical to 20_engine_core/emit.py
    h = hashlib.sha256()
    for jf in sorted(d.rglob("*.json")):
        if jf.name in SKIP: continue
        h.update(jf.relative_to(d).as_posix().encode()); h.update(jf.read_bytes())
    return h.hexdigest()[:16]

def ident(s):
    s = re.sub(r"[^a-zA-Z0-9_]", "_", s) or "_x"
    if s[0].isdigit(): s = "_" + s
    if s.startswith("__") or (s[0] == "_" and len(s) > 1 and s[1].isupper()): s = "j" + s
    return s + "_" if (s in CPP_KW or s in MACROS) else s

PRIM = {"Z": "bool", "B": "int8_t", "C": "uint16_t", "S": "int16_t", "I": "int32_t", "J": "int64_t", "F": "float", "D": "double"}
ARR_T = {"[": "uint8_t", "]": "int32_t", "q": "int64_t", "f": "float", "d": "double", "b": "uint8_t", "c": "uint16_t", "h": "int16_t"}
BOXED = {"java/lang/Boolean": ("bool", "box_bool"), "java/lang/Byte": ("int8_t", "box_i8"), "java/lang/Short": ("int16_t", "box_i16"),
         "java/lang/Character": ("uint16_t", "box_char"), "java/lang/Integer": ("int32_t", "box_i32"), "java/lang/Long": ("int64_t", "box_i64"),
         "java/lang/Float": ("float", "box_f32"), "java/lang/Double": ("double", "box_f64")}
ARR_RET = {"byte[]": ("std::vector<uint8_t>", "::stratum::arr_vec<uint8_t>(out)"), "int[]": ("std::vector<int32_t>", "::stratum::arr_vec<int32_t>(out)"),
           "long[]": ("std::vector<int64_t>", "::stratum::arr_vec<int64_t>(out)"), "float[]": ("std::vector<float>", "::stratum::arr_vec<float>(out)"),
           "double[]": ("std::vector<double>", "::stratum::arr_vec<double>(out)"), "boolean[]": ("std::vector<bool>", "::stratum::arr_bool(out)"),
           "char[]": ("std::vector<uint16_t>", "::stratum::arr_vec<uint16_t>(out)"), "short[]": ("std::vector<int16_t>", "::stratum::arr_vec<int16_t>(out)"),
           "java.lang.String[]": ("std::vector<std::string>", "::stratum::arr_strings(out)")}
OBJ_VEC = ("std::vector< ::stratum::Object>", "::stratum::arr_objs(out)")

def P(type_, holder, arg, key, cb=False): return dict(type=type_, holder=holder, arg=arg, key=key, cb=cb)

def param_spec(p, tag, i, wrap_bool):
    n = f"a{i}"
    if tag in PRIM:
        if tag == "Z" and wrap_bool: return P("::stratum::Bool", None, f"::stratum::to_arg({n})", "Bool")
        return P(PRIM[tag], None, f"::stratum::to_arg({n})", PRIM[tag])
    if tag == "s": return P("::stratum::JStr", None, f"{n}.arg()", "JStr")
    if tag in ARR_T: t = ARR_T[tag]; return P(f"::stratum::Arr<{t}>", None, f"{n}.arg()", f"Arr<{t}>")
    if tag == "T": return P("const ::stratum::StrArr&", None, f"{n}.arg()", "StrArr")
    if tag == "A": return P("const ::stratum::ObjArr&", None, f"{n}.arg()", "ObjArr")
    if tag == "M": return P("const ::stratum::ListArg&", None, f"{n}.arg()", "ListArg")
    if tag == "N": return P("const ::stratum::MapArg&", None, f"{n}.arg()", "MapArg")
    if tag in ("a", "p"):
        return P(f"C{i}&&", f"auto h{i} = ::stratum::make_callback(std::forward<C{i}>({n}));", f"h{i}.arg()", "CB", True)
    if tag == "L" and p.get("is_boxed") and p.get("jni_class") in BOXED:
        t, fn = BOXED[p["jni_class"]]
        if t == "bool" and wrap_bool: t = "::stratum::Bool"; fnarg = f"{n}.v"
        else: fnarg = n
        return P(t, f"::stratum::Object h{i} = ::stratum::{fn}({fnarg});", f"::stratum::to_arg(h{i})", t)
    return P("const ::stratum::Object&", None, f"::stratum::to_arg({n})", "Object")

def ret_spec(ret_id, ret_java):
    simple = {0: ("void", "v", None), 1: ("bool", "z", "return r != 0;"), 2: ("int8_t", "i", "return (int8_t)r;"), 3: ("uint16_t", "i", "return (uint16_t)r;"),
              4: ("int16_t", "i", "return (int16_t)r;"), 5: ("int32_t", "i", "return (int32_t)r;"), 6: ("int64_t", "j", "return r;"),
              7: ("float", "d", "return (float)r;"), 8: ("double", "d", "return r;"),
              9: ("std::string", "str", "return ::stratum::to_std(r);")}
    if ret_id in simple: return simple[ret_id]
    if ret_id == 11:
        t, e = ARR_RET.get(ret_java, OBJ_VEC); return t, "arr", f"return {e};"
    if ret_id == 12: return OBJ_VEC[0], "list", f"return {OBJ_VEC[1]};"
    return "::stratum::Object", "o", "return ::stratum::Object::adopt(r);"      # 10, 13 (Map)

FLT = r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?"
STR_OK = re.compile(r'^"(?:[^"\\]|\\[\\"nrt0\'])*"$')
def _fl(v): return v if re.search(r"[.eE]", v) else v + ".0"
def const_literal(f):
    if not (f.get("is_static") and f.get("is_final")): return None
    v, d = f.get("constant_value"), f.get("descriptor") or f.get("jni_signature", "")
    if v is None: return None
    v = v.strip()
    if d in ("I", "S", "B") and re.fullmatch(r"-?\d+", v):
        if d == "I" and int(v) == -2**31: return "int32_t", "(-2147483647-1)"
        return {"I": "int32_t", "S": "int16_t", "B": "int8_t"}[d], v
    if d == "J" and re.fullmatch(r"-?\d+[lL]?", v):
        n = int(v.rstrip("lL"))
        return ("int64_t", "(-9223372036854775807LL-1)") if n == -2**63 else ("int64_t", f"{n}LL")
    if d == "Z" and v in ("true", "false"): return "bool", v
    if d == "F" and re.fullmatch(FLT + r"[fF]?", v): return "float", _fl(v.rstrip("fF")) + "f"
    if d == "D" and re.fullmatch(FLT + r"[dD]?", v): return "double", _fl(v.rstrip("dD"))
    if d == "Ljava/lang/String;" and STR_OK.match(v): return "const char*", v
    return None

def sigkey(m):
    s = m.get("jni_signature", ""); return s[: s.find(")") + 1] if ")" in s else s

def collect_methods(allc, data):
    """own + ancestors + all interfaces (+ java.lang.Object); child overrides win; statics only from the class itself."""
    seen, out = set(), []
    def add(d, own):
        for m in d.get("methods", []):
            if m.get("is_constructor") or m.get("is_private"): continue
            if m.get("is_static") and not own: continue
            k = (m["name"], sigkey(m), bool(m.get("is_static")))
            if k in seen: continue
            seen.add(k); out.append((m, d["class_id"]))
    add(data, True)
    chain, p, guard = [data], data.get("parent_fqn", ""), set()
    while p and p in allc and p not in guard:
        guard.add(p); chain.append(allc[p]); add(allc[p], False); p = allc[p].get("parent_fqn", "")
    ifaces, q = [], [i for d in chain for i in d.get("interfaces", [])]
    seen_i = set()
    while q:
        i = q.pop(0)
        if i in seen_i or i not in allc: continue
        seen_i.add(i); add(allc[i], False); q += allc[i].get("interfaces", [])
    obj = allc.get("java.lang.Object")
    if obj is not None and obj is not data: add(obj, False)
    return out

def simple_t(p):
    return re.sub(r"[^a-zA-Z0-9]", "", (p.get("java_type", "obj").split(".")[-1]).replace("[]", "Arr")) or "obj"

def assign_names(entries, cname):
    """Unique C++ names per (name, C++ signature). Collisions get a type-suffix."""
    for e in entries:
        b = ident(e["name"]); e["base"] = b + "_" if (b in RESERVED or b == cname) else b
    used_sig, used_name = set(), set()
    for e in entries:
        sig = (e["base"], tuple(s["key"] for s in e["specs"]), e["static"])
        name = e["base"]
        if sig in used_sig:
            name = e["base"] + "_" + "_".join(simple_t(p) for p in e["params"]) if e["params"] else e["base"] + "_0"
            k, n = name, 2
            while (k, tuple(s["key"] for s in e["specs"]), e["static"]) in used_sig: k = f"{name}_{n}"; n += 1
            name = k
        used_sig.add((name, tuple(s["key"] for s in e["specs"]), e["static"])); used_sig.add(sig)
        e["name"] = name; used_name.add(name)
    return used_name

def body(L, kind, target, cid, slot, specs, rtype, rconv, ctor_wrap=None):
    for s in specs:
        if s["holder"]: L.append("        " + s["holder"])
    n = len(specs)
    if n: L.append(f"        const ::StratumArg args[] = {{ {', '.join(s['arg'] for s in specs)} }};")
    ap = "args" if n else "nullptr"
    L.append("        int32_t err = 0;")
    if kind == "ctor":
        L.append(f"        {ctor_wrap}stratum_new_instance({cid}, {slot}, {ap}, &err);")
        L.append("        ::stratum::check(err);"); return
    if kind == "v":
        L.append(f"        stratum_call_v({target}, {cid}, {slot}, {ap}, &err);"); L.append("        ::stratum::check(err);"); return
    if kind in ("arr", "list"):
        L.append("        ::StratumArr out{0, 0, nullptr};")
        L.append(f"        stratum_call_{kind}({target}, {cid}, {slot}, {ap}, &out, &err);")
    else:
        L.append(f"        auto r = stratum_call_{kind}({target}, {cid}, {slot}, {ap}, &err);")
    L.append("        ::stratum::check(err);"); L.append(f"        {rconv}")

def emit_class(allc, data, names):
    fqn, cid = data["fqn"], data["class_id"]
    pkg, cname = names[fqn]
    ns = "::".join(["stratum"] + pkg)
    no_ctor = bool(data.get("is_abstract") or data.get("is_interface") or data.get("is_annotation") or data.get("is_enum"))
    api = []
    L = [f"// Auto-generated by Stratum Stage 21 for {fqn}. DO NOT EDIT.", "#pragma once", "#include <stratum/core.hpp>", "",
         f"namespace {ns} {{", "", f"class {cname} : public ::stratum::Object {{", "public:",
         f"    static constexpr uint32_t _CLASS_ID = {cid};", f'    static constexpr const char* _FQN = "{fqn}";',
         f"    {cname}() noexcept = default;                       // NULL object",
         f"    static {cname} _wrap(int64_t h) noexcept {{ {cname} t; t.ptr_ = h; return t; }}  // adopts a global ref", ""]

    def mk_entries(items, is_ctor):
        es = []
        for m, mcid in items:
            tags, params = m.get("param_tags", ""), m.get("params", [])
            if len(params) > 32 or len(tags) != len(params): continue
            es.append(dict(m=m, cid=mcid, tags=tags, params=params, name=m["name"], static=bool(m.get("is_static"))))
        has_str = defaultdict(set)
        for e in es:
            for i, t in enumerate(e["tags"]):
                if t == "s": has_str[(e["name"], len(e["tags"]))].add(i)
        for e in es:
            e["specs"] = [param_spec(p, t, i, i in has_str[(e["name"], len(e["tags"]))]) for i, (t, p) in enumerate(zip(e["tags"], e["params"]))]
        return es

    # constructors
    if not no_ctor:
        ces = mk_entries([(m, cid) for m in data.get("methods", []) if m.get("is_constructor") and not m.get("is_private")], True)
        seen = set()
        for e in ces:
            key = tuple(s["key"] for s in e["specs"])
            as_ctor = bool(e["specs"]) and not any(s["cb"] for s in e["specs"]) and key not in seen
            seen.add(key)
            decls = ", ".join(f"{s['type']} a{i}" for i, s in enumerate(e["specs"]))
            tmpl = ", ".join(f"class C{i}" for i, s in enumerate(e["specs"]) if s["cb"])
            if tmpl: L.append(f"    template <{tmpl}>")
            if as_ctor:
                L.append(f"    explicit {cname}({decls}) {{"); body(L, "ctor", None, cid, e["m"]["slot"], e["specs"], None, None, "ptr_ = ")
                api.append(f"{fqn}: {cname}({decls})")
            else:
                suffix = "_" + "_".join(simple_t(p) for p in e["params"]) if e["params"] else ""
                nm = "create" if not e["params"] else f"create{suffix}"
                L.append(f"    static {cname} {nm}({decls}) {{ {cname} t;"); L.append("        {")
                inner = []; body(inner, "ctor", None, cid, e["m"]["slot"], e["specs"], None, None, "t.ptr_ = "); L += ["    " + x for x in inner]
                L += ["        }", "        return t;"]
                api.append(f"{fqn}: static {nm}({decls})")
            L.append("    }"); L.append("")

    # methods (flattened)
    es = mk_entries(collect_methods(allc, data), False)
    used = assign_names(es, cname)
    for e in es:
        m, specs = e["m"], e["specs"]
        rtype, kind, rconv = ret_spec(m.get("ret_type_id", 10), m.get("return_java_type", ""))
        decls = ", ".join(f"{s['type']} a{i}" for i, s in enumerate(specs))
        tmpl = ", ".join(f"class C{i}" for i, s in enumerate(specs) if s["cb"])
        if tmpl: L.append(f"    template <{tmpl}>")
        L.append(f"    {'static ' if e['static'] else ''}{rtype} {e['name']}({decls}){'' if e['static'] else ' const'} {{")
        body(L, kind, "0" if e["static"] else "ptr_", e["cid"], m["slot"], specs, rtype, rconv)
        L.append("    }"); L.append("")
        api.append(f"{fqn}: {e['name']}({decls}) -> {rtype}")

    # fields (own class only; inherited static constants come through ancestors' own headers via their class)
    allf = [(f, cid) for f in data.get("fields", [])]
    p, g = data.get("parent_fqn", ""), set()
    while p and p in allc and p not in g:
        g.add(p); allf += [(f, allc[p]["class_id"]) for f in allc[p].get("fields", [])]; p = allc[p].get("parent_fqn", "")
    seenf = set()
    for f, fcid in allf:
        if f.get("is_private") or f["name"] in seenf: continue
        seenf.add(f["name"])
        fslot, rid, is_static = f.get("slot", 0), f.get("ret_type_id", 10), bool(f.get("is_static"))
        c = const_literal(f); cn = ident(f["name"])
        if c and cn not in used and re.fullmatch(r"[A-Za-z_]\w*", cn):
            used.add(cn); L.append(f"    static constexpr {c[0]} {cn} = {c[1]};"); L.append(""); api.append(f"{fqn}: const {cn}"); continue
        nm = ident(f["name"]); pf = "sf" if is_static else "f"; tgt = "0" if is_static else "ptr_"
        rt, kind, rconv = ret_spec(rid, f.get("java_type", ""))
        fk = {"v": None, "z": "z", "i": "i", "j": "j", "d": "d", "str": "str", "o": "o", "arr": "arr", "list": "o"}[kind]
        if fk:
            if kind == "list": rt, rconv = "::stratum::Object", "return ::stratum::Object::adopt(r);"
            L.append(f"    {'static ' if is_static else ''}{rt} {pf}_get_{nm}(){'' if is_static else ' const'} {{")
            L.append("        int32_t err = 0;")
            if fk == "arr":
                L += ["        ::StratumArr out{0, 0, nullptr};", f"        stratum_field_get_arr({tgt}, {fcid}, {fslot}, &out, &err);"]
            else:
                L.append(f"        auto r = stratum_field_get_{fk}({tgt}, {fcid}, {fslot}, &err);")
            L += ["        ::stratum::check(err);", f"        {rconv}", "    }", ""]
            api.append(f"{fqn}: {pf}_get_{nm}() -> {rt}")
        if not f.get("is_final"):
            st = {1: ("bool", "z", "v ? 1 : 0"), 2: ("int8_t", "i", "v"), 3: ("uint16_t", "i", "v"), 4: ("int16_t", "i", "v"), 5: ("int32_t", "i", "v"),
                  6: ("int64_t", "j", "v"), 7: ("double", "d", "v"), 8: ("double", "d", "v"),
                  9: ("::stratum::JStr", "str", None), 10: ("const ::stratum::Object&", "o", "v.raw()")}.get(rid)
            if st:
                L.append(f"    {'static ' if is_static else ''}void {pf}_set_{nm}({st[0]} v){'' if is_static else ' const'} {{")
                L.append("        int32_t err = 0;")
                if st[1] == "str":
                    L += ["        ::StratumStr s = *static_cast<const ::StratumStr*>(v.arg().p);", f"        stratum_field_set_str({tgt}, {fcid}, {fslot}, &s, &err);"]
                else:
                    L.append(f"        stratum_field_set_{st[1]}({tgt}, {fcid}, {fslot}, {st[2]}, &err);")
                L += ["        ::stratum::check(err);", "    }", ""]
    L += ["};", "", f"}} // namespace {ns}"]
    return "\n".join(L), api

def main():
    ap = argparse.ArgumentParser(description="Stratum Stage 21 - C++ SDK emitter")
    ap.add_argument("--input", required=True, help="05_resolve/output_patched/")
    ap.add_argument("--output", default=None,
                    help="Output dir (any location). Default: <this folder>/output")
    ap.add_argument("--emit-prefix", default="", help="comma-separated FQN prefixes (empty = everything; large). android.app.Activity is always emitted.")
    a = ap.parse_args()
    inp = Path(str(a.input).strip(' "\'')).resolve()
    out = Path(str(a.output).strip(' "\'')).resolve() if a.output else HERE / "output"
    inc = out / "include"; sd = inc / "stratum"
    if sd.exists(): shutil.rmtree(sd)
    sd.mkdir(parents=True)
    for n in ("core.hpp", "stratum.hpp", "reflect.hpp"): shutil.copy2(HERE / "templates" / n, sd / n)
    shutil.copy2(ENGINE_INC / "stratum_c_abi.h", inc / "stratum_c_abi.h")
    (sd / "meta_hash.hpp").write_text(f'#pragma once\n#define STRATUM_META_HASH "{meta_hash(inp)}"\n', encoding="utf-8")

    allc = {}
    for jf in sorted(inp.rglob("*.json")):
        if jf.name in SKIP: continue
        d = json.loads(jf.read_text(encoding="utf-8"))
        if d.get("fqn") and "class_id" in d and "." in d["fqn"]: allc[d["fqn"]] = d
    if not allc: raise SystemExit("No resolved classes.")
    pre = [p.strip() for p in a.emit_prefix.split(",") if p.strip()]
    emit = {f: d for f, d in allc.items() if not d.get("is_annotation")
            and (not pre or any(f.startswith(p) for p in pre) or f == "android.app.Activity")}
    if "android.app.Activity" not in emit: raise SystemExit("android.app.Activity missing from input")

    kids = defaultdict(set)
    for f in emit:
        parts = [ident(p) for p in f.rsplit(".", 1)[0].split(".")]
        for i in range(len(parts)): kids[tuple(parts[:i])].add(parts[i])
    names = {}
    for f in emit:
        parts = tuple(ident(p) for p in f.rsplit(".", 1)[0].split("."))
        c = ident(f.rsplit(".", 1)[1].replace("$", "_"))
        if c in kids[parts]: c += "_"
        names[f] = (list(parts), c)

    index, n = [], 0
    for f, d in emit.items():
        code, api = emit_class(allc, d, names)
        pkg, c = names[f]
        dest = sd.joinpath(*pkg); dest.mkdir(parents=True, exist_ok=True)
        (dest / f"{c}.hpp").write_text(code, encoding="utf-8"); index += api; n += 1
    (sd / "API_INDEX.txt").write_text("\n".join(sorted(index)), encoding="utf-8")
    print(f"-> {n:,} class headers, {len(index):,} API entries -> {inc}")

if __name__ == "__main__":
    main()