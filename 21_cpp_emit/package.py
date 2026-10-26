#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 21b: assemble the self-contained SDK folder `stratum/` (source only, no build).

    stratum/
    ├── README.md
    ├── cpp/                      -> merge into app/src/main/cpp/
    │   ├── CMakeLists.txt, native-lib.cpp
    │   └── stratum/              engine (CMakeLists.txt, src/, generated/, include/)
    └── stratum-runtime/          -> copy to <project root>/stratum-runtime
        ├── build.gradle (rendered), consumer-rules.pro
        └── src/main/{AndroidManifest.xml, java/com/stratum/{runtime,adapters}}
"""
import argparse, re, shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def die(msg):
    sys.exit("ERROR: " + msg)


def need(p: Path, what: str):
    if not p.exists():
        die(f"{what} not found: {p}")


def copy_files(src: Path, dst: Path, patterns):
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for pat in patterns:
        for f in sorted(src.glob(pat)):
            if f.is_file():
                shutil.copy2(f, dst / f.name)
                n += 1
    return n


def read_hash(path: Path) -> str:
    m = re.search(r'STRATUM_META_HASH\s+"([0-9a-f]+)"', path.read_text(encoding="utf-8"))
    if not m:
        die(f"no STRATUM_META_HASH in {path}")
    return m.group(1)


def render(src: Path, dst: Path, values: dict):
    text = src.read_text(encoding="utf-8")
    for k, v in values.items():
        text = text.replace(f"@@{k}@@", str(v))
    left = re.findall(r"@@[A-Z_]+@@", text)
    if left:
        die(f"unresolved placeholders {sorted(set(left))} in {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")


def java_const(v: str) -> str:
    v = str(v).strip()
    return "VERSION_1_8" if v in ("8", "1.8") else f"VERSION_{v}"


def main():
    ap = argparse.ArgumentParser(description="Stratum Stage 21b - package SDK source folder")
    _clean = lambda p: Path(str(p).strip(' "\'')).resolve()
    ap.add_argument("--engine", default=str(ROOT / "20_engine_core"))
    ap.add_argument("--generated", default=None,
                    help="metadata_table.cpp/.h + meta_hash.h folder (default: <engine>/generated)")
    ap.add_argument("--cpp-out", "--input", dest="cpp_out", default=None,
                    help="Stage 21 main.py output (has include/). Default: <this folder>/output")
    ap.add_argument("--runtime", default=str(ROOT / "runtime"),
                    help="runtime/ folder containing stratum-runtime/ and templates/")
    ap.add_argument("--adapters", default=str(ROOT / "05_5_abstract" / "output" / "java" / "com" / "stratum" / "adapters"))
    ap.add_argument("--output", default=None, help="Packaged SDK folder. Default: <cpp-out>/stratum")
    ap.add_argument("--api-version", required=True, help="Android API the SDK was generated from (-> compileSdk)")
    ap.add_argument("--min-sdk", default="24", help="minSdk of the runtime module")
    ap.add_argument("--java-version", default="1.8", help="Java level of the runtime module (1.8, 11, 17)")
    a = ap.parse_args()

    engine, runtime = _clean(a.engine), _clean(a.runtime)
    cpp_out = _clean(a.cpp_out) if a.cpp_out else (HERE / "output").resolve()
    generated = _clean(a.generated) if a.generated else engine / "generated"
    adapters = _clean(a.adapters)
    out = _clean(a.output) if a.output else cpp_out / "stratum"

    rt_mod, tpl = runtime / "stratum-runtime", runtime / "templates"
    need(engine / "CMakeLists.txt", "engine CMakeLists.txt")
    need(engine / "src", "engine src/")
    need(generated / "metadata_table.cpp", "generated/metadata_table.cpp (run 20_engine_core/emit.py)")
    need(generated / "meta_hash.h", "generated/meta_hash.h")
    need(engine / "include" / "stratum_c_abi.h", "stratum_c_abi.h")
    need(cpp_out / "include" / "stratum" / "core.hpp", "C++ headers (run 21_cpp_emit/main.py)")
    need(cpp_out / "include" / "stratum" / "meta_hash.hpp", "meta_hash.hpp")
    need(rt_mod / "src" / "main" / "java", "runtime/stratum-runtime/src/main/java")
    need(rt_mod / "src" / "main" / "AndroidManifest.xml", "runtime/stratum-runtime/src/main/AndroidManifest.xml")
    need(rt_mod / "consumer-rules.pro", "runtime/stratum-runtime/consumer-rules.pro")
    need(rt_mod / "build.gradle.template", "runtime/stratum-runtime/build.gradle.template")
    for t in ("CMakeLists.txt", "native-lib.cpp", "SDK_README.md"):
        need(tpl / t, f"runtime/templates/{t}")

    h_engine = read_hash(generated / "meta_hash.h")
    h_sdk = read_hash(cpp_out / "include" / "stratum" / "meta_hash.hpp")
    if h_engine != h_sdk:
        die(f"meta hash mismatch: engine={h_engine} headers={h_sdk}. "
            "Re-run 20_engine_core/emit.py and 21_cpp_emit/main.py from the SAME input.")

    # never delete a folder we are reading from
    if out in (cpp_out, cpp_out / "include"):
        die("--output must not be the same as --cpp-out or its include/ dir")
    for _src in (engine, cpp_out, runtime, adapters, generated):
        if out == _src or out in _src.parents:
            die(f"--output ({out}) must not equal or contain a source folder: {_src}")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    values = dict(COMPILE_SDK=a.api_version, MIN_SDK=a.min_sdk,
                  JAVA_VERSION=java_const(a.java_version), META_HASH=h_engine)

    # ── cpp/ : app-side CMake + demo + engine ────────────────────────────────
    cpp_dir = out / "cpp"
    eng_dir = cpp_dir / "stratum"
    eng_dir.mkdir(parents=True, exist_ok=True)          # also creates cpp_dir
    shutil.copy2(tpl / "CMakeLists.txt", cpp_dir / "CMakeLists.txt")
    shutil.copy2(tpl / "native-lib.cpp", cpp_dir / "native-lib.cpp")
    shutil.copy2(engine / "CMakeLists.txt", eng_dir / "CMakeLists.txt")
    n_src = copy_files(engine / "src", eng_dir / "src", ["*.cpp", "*.h", "*.hpp"])
    n_gen = copy_files(generated, eng_dir / "generated", ["*.cpp", "*.h"])
    shutil.copytree(cpp_out / "include", eng_dir / "include", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(engine / "include" / "stratum_c_abi.h", eng_dir / "include" / "stratum_c_abi.h")

    # ── stratum-runtime/ : Android library module ───────────────────────────
    mod = out / "stratum-runtime"
    shutil.copytree(rt_mod, mod, ignore=shutil.ignore_patterns(
        "__pycache__", "build", "*.iml", "build.gradle.template", "build.gradle"))
    render(rt_mod / "build.gradle.template", mod / "build.gradle", values)
    n_ad = 0
    if adapters.exists():
        n_ad = copy_files(adapters, mod / "src" / "main" / "java" / "com" / "stratum" / "adapters", ["*.java"])
    else:
        print(f"-> note: no adapters dir at {adapters} (Stage 05.5 off?) - skipped")

    render(tpl / "SDK_README.md", out / "README.md", values)

    n_hpp = len(list((eng_dir / "include").rglob("*.hpp")))
    print("=" * 60)
    print(f" STRATUM SDK PACKAGED  (meta hash {h_engine}, compileSdk {a.api_version})")
    print("=" * 60)
    print(f"-> engine sources : {n_src} files, generated: {n_gen} files")
    print(f"-> C++ headers    : {n_hpp} .hpp")
    print(f"-> java adapters  : {n_ad}")
    print(f"-> output         : {out}")
    print("   cpp/ -> app/src/main/cpp/    stratum-runtime/ -> <project root>/stratum-runtime")


if __name__ == "__main__":
    main()