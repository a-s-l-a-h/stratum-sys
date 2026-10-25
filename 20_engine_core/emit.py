#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage 20a: metadata_table.h/.cpp + meta_hash.h from 05_resolve/output_patched/ (same table as Stage 10)."""
import argparse, hashlib, json
from pathlib import Path

SKIP = {"parse_summary.json", "resolve_summary.json", "manifest.json", "sanitize_summary.json"}

def meta_hash(input_dir: Path) -> str:        # MUST be identical in 21_cpp_emit/main.py
    h = hashlib.sha256()
    for jf in sorted(input_dir.rglob("*.json")):
        if jf.name in SKIP: continue
        h.update(jf.relative_to(input_dir).as_posix().encode()); h.update(jf.read_bytes())
    return h.hexdigest()[:16]

class StringPool:
    def __init__(self): self.pool = bytearray(b"\x00"); self.offsets = {"": 0}
    def get_offset(self, s):
        if not s: return 0
        if s in self.offsets: return self.offsets[s]
        off = len(self.pool); self.pool.extend(s.encode("utf-8") + b"\x00"); self.offsets[s] = off
        return off

def emit_metadata_table(classes, pool):
    cpp = ["// metadata_table.cpp - Stratum auto-generated. DO NOT EDIT.", '#include "metadata_table.h"', ""]
    for cls in classes:
        cid = cls["class_id"]; methods, fields = cls.get("methods", []), cls.get("fields", [])
        if methods:
            cpp.append(f"static const MethodMeta g_methods_cls_{cid}[] = {{")
            for m in methods:
                nv = "<init>" if m.get("is_constructor") else m.get("name", "")
                cpp.append("    {%d, %d, %d, %d, %d, %d, %d, %d}," % (
                    pool.get_offset(nv), pool.get_offset(m.get("jni_signature", "")), pool.get_offset(m.get("param_tags", "")),
                    pool.get_offset(m.get("adapter_jni", "")), m.get("ret_type_id", 0), 1 if m.get("is_static") else 0,
                    1 if m.get("is_constructor") else 0, len(m.get("params", []))))
            cpp += ["};", ""]
        if fields:
            cpp.append(f"static const FieldMeta g_fields_cls_{cid}[] = {{")
            for f in fields:
                cpp.append("    {%d, %d, %d, %d}," % (
                    pool.get_offset(f.get("name", "")), pool.get_offset(f.get("jni_signature", f.get("jni_type", ""))),
                    f.get("ret_type_id", 10), 1 if f.get("is_static") else 0))
            cpp += ["};", ""]
    cpp.append(f"ClassMeta g_classes[{len(classes)}] = {{")
    for cls in classes:
        cid = cls["class_id"]
        mp = f"g_methods_cls_{cid}" if cls.get("method_count", 0) else "nullptr"
        fp = f"g_fields_cls_{cid}" if cls.get("field_count", 0) else "nullptr"
        cpp.append("    {%d, %d, %d, %s, %s, nullptr, nullptr, nullptr}," % (
            pool.get_offset(cls.get("jni_name", "")), cls.get("method_count", 0), cls.get("field_count", 0), mp, fp))
    cpp += ["};", "", f"extern const uint32_t g_class_count = {len(classes)};", "",
            f"extern const char g_str_pool[{len(pool.pool)}] = {{"]
    hexb = [f"0x{b:02x}" for b in pool.pool]
    for i in range(0, len(hexb), 16): cpp.append("    " + ", ".join(hexb[i:i + 16]) + ",")
    cpp.append("};")
    h = ["// metadata_table.h - Stratum auto-generated. DO NOT EDIT.", "#pragma once",
         "#include <jni.h>", "#include <stdint.h>", "#include <atomic>", "",
         "struct MethodMeta { uint32_t name_offset; uint32_t sig_offset; uint32_t tags_offset; uint32_t adapter_jni_offset;",
         "                    uint8_t ret_type; uint8_t is_static; uint8_t is_constructor; uint8_t param_count; };",
         "struct FieldMeta { uint32_t name_offset; uint32_t sig_offset; uint8_t type_id; uint8_t is_static; };",
         "struct ClassMeta {", "    uint32_t jni_name_offset; uint16_t method_count; uint16_t field_count;",
         "    const MethodMeta* methods; const FieldMeta* fields;",
         "    jclass class_ref; jmethodID* method_ids; jfieldID* field_ids;",
         "    std::atomic<bool> resolved{false};", "};", "",
         f"extern const char g_str_pool[{len(pool.pool)}];", f"extern ClassMeta g_classes[{len(classes)}];",
         "extern const uint32_t g_class_count;"]
    return "\n".join(h), "\n".join(cpp)

def main():
    ap = argparse.ArgumentParser(description="Stratum Stage 20 - metadata emit")
    ap.add_argument("--input", required=True, help="05_resolve/output_patched/")
    ap.add_argument("--output", default=None,
                    help="Output dir (any location). Default: <this folder>/generated")
    a = ap.parse_args()
    inp = Path(str(a.input).strip(' "\'')).resolve()
    out = (Path(str(a.output).strip(' "\'')).resolve() if a.output
           else Path(__file__).resolve().parent / "generated")
    out.mkdir(parents=True, exist_ok=True)
    classes = []
    for jf in sorted(inp.rglob("*.json")):
        if jf.name in SKIP: continue
        d = json.loads(jf.read_text(encoding="utf-8"))
        if d.get("fqn") and "class_id" in d: classes.append(d)
    if not classes: raise SystemExit("No resolved classes - run 05_resolve pass 2 first.")
    classes.sort(key=lambda c: c["class_id"])
    pool = StringPool(); h, c = emit_metadata_table(classes, pool)
    (out / "metadata_table.h").write_text(h, encoding="utf-8")
    (out / "metadata_table.cpp").write_text(c, encoding="utf-8")
    hx = meta_hash(inp)
    (out / "meta_hash.h").write_text(f'#pragma once\n#define STRATUM_META_HASH "{hx}"\n', encoding="utf-8")
    print(f"-> {len(classes):,} classes, string pool {len(pool.pool)/1024:.1f} KB, meta hash {hx}")

if __name__ == "__main__":
    main()