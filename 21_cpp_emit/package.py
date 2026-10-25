#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage 21b: assemble the self-contained `stratum/` SDK folder (source only, no build).

Run order:
    python 20_engine_core/emit.py --input 05_resolve/output_patched
    python 21_cpp_emit/main.py    --input 05_resolve/output_patched [--emit-prefix ...]
    python 21_cpp_emit/package.py

Result: 21_cpp_emit/output/stratum/  -> copy into app/src/main/cpp/stratum/
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


README = """# Stratum C++ SDK (source)

Everything needed to use Stratum from C++ in an Android Studio project.

## Setup
1. Copy this whole `stratum/` folder to `app/src/main/cpp/stratum/`.
2. `app/src/main/cpp/CMakeLists.txt`:

       add_subdirectory(stratum)
       add_library(myapp SHARED my_code.cpp)
       find_library(log-lib log)
       target_link_libraries(myapp PRIVATE stratum ${log-lib})

3. `app/build.gradle(.kts)`:
   - externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
   - add `src/main/cpp/stratum/java` as a Java source dir
   - dependency: androidx.appcompat:appcompat
4. Merge `android/AndroidManifest.snippet.xml` into your manifest only if you use
   StratumService / StratumReceiver.
5. Your Activity extends `com.stratum.runtime.StratumActivity` and loads YOUR native
   library in `onImportMain()` (before lifecycle events are dispatched).
6. In C++: `#include <stratum/stratum.hpp>` and define `void onCreate(stratum::Activity act)`.

Options: -DSTRATUM_LOG_ENABLED=1 compiles in tracing (toggle with stratum::set_log_enabled).
"""


def main():
    ap = argparse.ArgumentParser(description="Stratum Stage 21b - package SDK source folder")
    _clean = lambda p: Path(str(p).strip(' "\'')).resolve()
    ap.add_argument("--engine", default=str(ROOT / "20_engine_core"))
    ap.add_argument("--generated", default=None,
                    help="Folder with metadata_table.cpp/.h + meta_hash.h from 20_engine_core/emit.py "
                         "(default: <engine>/generated)")
    ap.add_argument("--cpp-out", "--input", dest="cpp_out", default=None,
                    help="Stage 21 main.py output (has include/). Default: <this folder>/output")
    ap.add_argument("--runtime", default=str(ROOT / "runtime"))
    ap.add_argument("--adapters", default=str(ROOT / "05_5_abstract" / "output" / "java" / "com" / "stratum" / "adapters"),
                    help="Folder with generated Adapter_*.java (05_5_abstract output/java/com/stratum/adapters)")
    ap.add_argument("--output", default=None,
                    help="Packaged SDK folder (any location). Default: <cpp-out>/stratum")
    a = ap.parse_args()

    engine, runtime = _clean(a.engine), _clean(a.runtime)
    cpp_out = _clean(a.cpp_out) if a.cpp_out else (HERE / "output").resolve()
    generated = _clean(a.generated) if a.generated else engine / "generated"
    adapters = _clean(a.adapters)
    out = _clean(a.output) if a.output else cpp_out / "stratum"

    need(engine / "CMakeLists.txt", "engine CMakeLists.txt")
    need(engine / "src", "engine src/")
    need(generated / "metadata_table.cpp", "generated/metadata_table.cpp (run 20_engine_core/emit.py)")
    need(generated / "meta_hash.h", "generated/meta_hash.h")
    need(engine / "include" / "stratum_c_abi.h", "stratum_c_abi.h")
    need(cpp_out / "include" / "stratum" / "core.hpp", "C++ headers (run 21_cpp_emit/main.py)")
    need(cpp_out / "include" / "stratum" / "meta_hash.hpp", "meta_hash.hpp")
    need(runtime / "java", "runtime/java")

    h_engine = read_hash(generated / "meta_hash.h")
    h_sdk = read_hash(cpp_out / "include" / "stratum" / "meta_hash.hpp")
    if h_engine != h_sdk:
        die(f"meta hash mismatch: engine={h_engine} headers={h_sdk}. "
            "Re-run 20_engine_core/emit.py and 21_cpp_emit/main.py from the SAME input.")

    # never delete the folder we are reading from
    if out.resolve() in (cpp_out.resolve(), cpp_out.resolve() / "include"):
        die("--output must not be the same as --cpp-out or its include/ dir")
    for _src in (engine, cpp_out, runtime, adapters, generated):
        _s = _src.resolve()
        if out.resolve() == _s or out.resolve() in _s.parents:
            die(f"--output ({out}) must not equal or contain a source folder: {_s}")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    shutil.copy2(engine / "CMakeLists.txt", out / "CMakeLists.txt")
    n_src = copy_files(engine / "src", out / "src", ["*.cpp", "*.h", "*.hpp"])
    n_gen = copy_files(generated, out / "generated", ["*.cpp", "*.h"])

    shutil.copytree(cpp_out / "include", out / "include",
                    ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(engine / "include" / "stratum_c_abi.h", out / "include" / "stratum_c_abi.h")

    shutil.copytree(runtime / "java", out / "java", ignore=shutil.ignore_patterns("__pycache__"))
    n_ad = 0
    if adapters.exists():
        n_ad = copy_files(adapters, out / "java" / "com" / "stratum" / "adapters", ["*.java"])
    else:
        print(f"-> note: no adapters dir at {adapters} (Stage 05.5 off?) - skipped")

    if (runtime / "AndroidManifest.xml").exists():
        (out / "android").mkdir()
        shutil.copy2(runtime / "AndroidManifest.xml", out / "android" / "AndroidManifest.snippet.xml")
    if (runtime / "consumer-rules.pro").exists():
        shutil.copy2(runtime / "consumer-rules.pro", out / "consumer-rules.pro")

    (out / "README.md").write_text(README, encoding="utf-8")

    n_hpp = len(list((out / "include").rglob("*.hpp")))
    print("=" * 60)
    print(f" STRATUM SDK PACKAGED  (meta hash {h_engine})")
    print("=" * 60)
    print(f"-> engine sources : {n_src} files, generated: {n_gen} files")
    print(f"-> C++ headers    : {n_hpp} .hpp")
    print(f"-> java adapters  : {n_ad}")
    print(f"-> output         : {out}")
    print("Copy that folder to <app>/src/main/cpp/stratum/")


if __name__ == "__main__":
    main()