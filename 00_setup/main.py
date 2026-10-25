import argparse
import json
import subprocess
import sys
import shutil
import platform
from pathlib import Path
from datetime import datetime
import re

# --- Helper Functions ---

def resolve_javap_executable(java_path_arg):
    """
    Finds and resolves javap to a cross-platform absolute path.
    Accepts either:
    1. Path to JDK root (e.g., 'D:/jdk-17' -> automatically looks in 'bin/')
    2. Direct path to javap (e.g., 'D:/jdk-17/bin/javap.exe')
    3. None (falls back to system PATH)
    """
    is_windows = platform.system() == "Windows"
    exe_name = "javap.exe" if is_windows else "javap"

    if java_path_arg:
        p = Path(java_path_arg).resolve()
        # Direct executable file
        if p.is_file():
            return str(p)
        if is_windows and p.with_suffix(".exe").is_file():
            return str(p.with_suffix(".exe"))
        # Directory passed (check <dir>/bin/javap or <dir>/javap)
        if p.is_dir():
            for candidate in (p / "bin" / exe_name, p / exe_name):
                if candidate.is_file():
                    return str(candidate)
        return None

    # Fallback to system PATH
    sys_path = shutil.which("javap")
    return str(Path(sys_path).resolve()) if sys_path else None

# --- Verification Functions ---

def check_python():
    """Ensure host Python is 3.10+ (pipeline stages use modern type syntax)"""
    v = sys.version_info
    version_str = f"{v.major}.{v.minor}.{v.micro}"
    if v.major == 3 and v.minor >= 10:
        return True, f"Found v{version_str}"
    return False, f"Found v{version_str}. Require Python 3.10+"

def check_javap(java_path_arg, expected_version=None):
    """Find javap, verify it executes, and optionally match against --jdk-version"""
    javap_path = resolve_javap_executable(java_path_arg)
    if not javap_path:
        return False, None, "javap executable not found. Pass --javap-path or add Java to PATH."

    try:
        result = subprocess.run([javap_path, "-version"], capture_output=True, text=True, check=True)
        output = result.stdout.strip() or result.stderr.strip()

        match = re.search(r'(\d+(?:\.\d+)*)', output)
        if not match:
            return False, javap_path, f"javap executed, but could not parse version from: '{output}'"

        version_str = match.group(1)
        major_version = version_str.split('.')[0]

        # If user specified an expected version (e.g., "17", "21", "17.0.2")
        if expected_version:
            exp = str(expected_version).strip()
            # Check major version match (e.g. exp="17" matches "17.0.20") OR prefix match
            if major_version == exp or version_str.startswith(exp):
                return True, javap_path, f"Found v{version_str} at {javap_path}"
            else:
                return False, javap_path, f"Found v{version_str} at {javap_path}, but does not match expected version '{exp}'"

        return True, javap_path, f"Found v{version_str} at {javap_path}"
    except Exception as e:
        return False, javap_path, f"Failed to execute javap at {javap_path}: {e}"

def check_android_jar(explicit_jar_path, api_version):
    """Ensure Android jar exists at the given path (any location accepted)"""
    if not explicit_jar_path:
        return False, None, "No path provided. Must provide --jar-path."

    jar_path = Path(explicit_jar_path).resolve()
    if jar_path.is_file():
        return True, str(jar_path), f"Found at {jar_path}"
    else:
        return False, None, f"android.jar not found at {jar_path}"

def check_optional_file(path_str, label):
    """Resolve and verify optional file path if provided"""
    if not path_str:
        return True, None, "Not provided (skipped)"
    p = Path(path_str).resolve()
    if p.is_file():
        return True, str(p), f"Found at {p}"
    return False, None, f"{label} not found at {p}"

# --- Main Execution ---

def main():
    parser = argparse.ArgumentParser(description="Stratum Stage 00 - Setup and Validation")
    parser.add_argument("--javap-path", "--javap_path", "--jdk-path", "--jdk_path", dest="java_path", type=str,
                        help="Path to javap executable OR JDK root directory (Optional if Java is on PATH)")
    parser.add_argument("--jdk-version", "--jdk_version", type=str, default=None, help="Optional expected JDK version to check (e.g. 17, 21)")
    parser.add_argument("--jar-path", "--jar_path", type=str, required=True, help="Direct path to android.jar (Any location, Relative or Absolute)")
    parser.add_argument("--api-version", "--api_version", type=str, default="35", help="Target Android API version (Default: 35)")
    parser.add_argument("--api-versions-xml-path", "--api_versions_xml_path", type=str, default=None,
                        help="Direct path to api-versions.xml (Optional)")
    parser.add_argument("--hiddenapi-path", "--hiddenapi_path", type=str, default=None,
                        help="Direct path to hiddenapi-flags.csv (Optional)")
    parser.add_argument("--output", type=str, required=True, help="Output directory for setup_report.json")

    # Deprecated flags: silently ignored so existing scripts don't break
    for old in ("--sdk-path", "--sdk_path", "--ndk-path", "--ndk_path", "--ndk-api", "--cmake-path", "--nanobind-version", "--python-target-version", "--py-version"):
        parser.add_argument(old, type=str, default=None, help=argparse.SUPPRESS)

    args = parser.parse_args()

    print("==================================================")
    print("      STRATUM PIPELINE - STAGE 00 (SETUP)         ")
    print("==================================================\n")

    errors = []

    py_ok, py_msg = check_python()
    print(f"[{'OK' if py_ok else 'FAIL'}] Python 3.10+   : {py_msg}")
    if not py_ok: errors.append(py_msg)

    javap_ok, javap_path, javap_msg = check_javap(args.java_path, args.jdk_version)
    print(f"[{'OK' if javap_ok else 'FAIL'}] Java (javap)   : {javap_msg}")
    if not javap_ok: errors.append(javap_msg)

    jar_ok, jar_path, jar_msg = check_android_jar(args.jar_path, args.api_version)
    print(f"[{'OK' if jar_ok else 'FAIL'}] Android API {args.api_version} : {jar_msg}")
    if not jar_ok: errors.append(jar_msg)

    xml_ok, api_versions_xml_path, xml_msg = check_optional_file(args.api_versions_xml_path, "api-versions.xml")
    if args.api_versions_xml_path:
        print(f"[{'OK' if xml_ok else 'FAIL'}] api-versions.xml : {xml_msg}")
        if not xml_ok: errors.append(xml_msg)

    hidden_ok, hiddenapi_path, hidden_msg = check_optional_file(args.hiddenapi_path, "hiddenapi-flags.csv")
    if args.hiddenapi_path:
        print(f"[{'OK' if hidden_ok else 'FAIL'}] hiddenapi-flags : {hidden_msg}")
        if not hidden_ok: errors.append(hidden_msg)

    all_ok = all([py_ok, javap_ok, jar_ok, xml_ok, hidden_ok])

    report = {
        "all_ok": all_ok,
        "javap_path": javap_path,                      # used by Stage 03
        "javap_version": javap_msg if javap_ok else None,
        "jdk_version": args.jdk_version,
        "jar_path": jar_path,                          # used by Stage 01
        "api_versions_xml_path": api_versions_xml_path,
        "hiddenapi_path": hiddenapi_path,
        "sdk_path": None,
        "android_api": args.api_version,               # used by Stage 01
        "python_version": py_msg,
        "ndk_path": None,
        "timestamp": datetime.now().isoformat()
    }

    out_dir = Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "setup_report.json"

    with open(out_file, "w") as f:
        json.dump(report, f, indent=2)

    print(f"\n-> Report written to: {out_file}\n")

    if not all_ok:
        print("==================================================")
        print(" SETUP FAILED: Please fix the following errors:")
        print("==================================================")
        for err in errors:
            print(f" X  {err}")
        print("\nExiting.")
        sys.exit(1)
    else:
        print("==================================================")
        print(" SETUP SUCCESS: All systems ready!")
        print("==================================================")

if __name__ == "__main__":
    main()