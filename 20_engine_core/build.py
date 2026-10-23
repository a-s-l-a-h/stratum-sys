#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage 20b: build libstratum.so (pure NDK, no Python). Rename-free: the file is already libstratum.so."""
import argparse, json, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).parent

def find_ninja(ndk: Path):
    try:
        import ninja
        for n in ("ninja.exe", "ninja"):
            p = Path(ninja.BIN_DIR) / n
            if p.exists(): return str(p)
    except ImportError: pass
    f = shutil.which("ninja")
    if f: return f
    for p in ndk.rglob("ninja*"):
        if p.is_file() and p.name in ("ninja", "ninja.exe"): return str(p)
    return None

def main():
    ap = argparse.ArgumentParser(description="Stratum Stage 20b - build libstratum.so")
    ap.add_argument("--setup", default="00_setup/output/setup_report.json")
    ap.add_argument("--abi", default="arm64-v8a", choices=["arm64-v8a", "armeabi-v7a", "x86_64", "x86"])
    ap.add_argument("--output", default="20_engine_core/output")
    ap.add_argument("--log", dest="log", action="store_true", help="compile in LOGD/LOGT tracing (runtime toggle stratum_set_log_enabled)")
    a = ap.parse_args()
    if not (HERE / "generated" / "metadata_table.cpp").exists():
        sys.exit("Run 20_engine_core/emit.py first.")
    setup = json.loads(Path(a.setup).read_text(encoding="utf-8"))
    ndk = Path(setup["ndk_path"]); cmake = setup.get("cmake_path", "cmake")
    api = str(setup.get("ndk_api", "24"))
    if api.isdigit() and int(api) > 33: api = "33"           # NDK r25c max compile target (same clamp as 10_embed)
    build = HERE / "build" / a.abi; out = Path(a.output) / a.abi; out.mkdir(parents=True, exist_ok=True)
    cmd = [cmake, str(HERE), f"-B{build}", f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
           f"-DANDROID_ABI={a.abi}", f"-DANDROID_PLATFORM=android-{api}", "-DANDROID_STL=c++_static",
           "-DCMAKE_BUILD_TYPE=Release", f"-DSTRATUM_LOG_ENABLED={1 if a.log else 0}", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"]
    nj = find_ninja(ndk)
    if nj: cmd += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={nj}"]
    subprocess.run(cmd, check=True)
    subprocess.run([cmake, "--build", str(build), "--parallel"], check=True)
    so = next(build.rglob("libstratum.so"))
    shutil.copy2(so, out / "libstratum.so")
    print(f"-> {out / 'libstratum.so'} ({so.stat().st_size/1024:.0f} KB)")

if __name__ == "__main__":
    main()