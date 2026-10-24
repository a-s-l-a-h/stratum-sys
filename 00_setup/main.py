import argparse
import json
import subprocess
import sys
import shutil
import platform
from pathlib import Path
from datetime import datetime

# --- Helper Functions ---

def get_executable(base_path, cmd_name):
    """Safely find an executable, fully cross-platform (Windows/Mac/Linux)"""
    is_windows = platform.system() == "Windows"
    exe_name = f"{cmd_name}.exe" if is_windows else cmd_name

    if base_path:
        target = Path(base_path) / "bin" / exe_name
        if target.exists():
            return str(target.resolve())

    sys_path = shutil.which(cmd_name)
    return str(Path(sys_path).resolve()) if sys_path else None

# --- Verification Functions ---

def check_python():
    """Ensure host Python is 3.10+ (pipeline stages use modern type syntax)"""
    v = sys.version_info
    version_str = f"{v.major}.{v.minor}.{v.micro}"
    if v.major == 3 and v.minor >= 10:
        return True, f"Found v{version_str}"
    return False, f"Found v{version_str}. Require Python 3.10+"

def check_javap(jdk_path):
    """Find javap and ensure it is version 17+"""
    javap_path = get_executable(jdk_path, "javap")
    if not javap_path:
        return False, None, "Command 'javap' not found. Ensure Java is installed or pass --jdk-path."

    try:
        result = subprocess.run([javap_path, "-version"], capture_output=True, text=True, check=True)
        output = result.stdout.strip() or result.stderr.strip()
        version_str = output.split()[1] if len(output.split()) > 1 else output

        major_version = int(version_str.split('.')[0])
        if major_version >= 17:
            return True, javap_path, f"Found v{version_str} at {javap_path}"
        else:
            return False, javap_path, f"Found v{version_str}. Require Java 17+."
    except Exception as e:
        return False, javap_path, f"Failed to execute javap: {e}"

def check_android_jar(sdk_path, explicit_jar_path, api_version):
    """Ensure Android jar exists for the requested API version"""
    if explicit_jar_path:
        jar_path = Path(explicit_jar_path)
    elif sdk_path:
        jar_path = Path(sdk_path) / "platforms" / f"android-{api_version}" / "android.jar"
    else:
        return False, None, "No path provided. Must provide --jar-path or --sdk-path."

    if jar_path.exists():
        return True, str(jar_path.resolve()), f"Found at {jar_path}"
    else:
        return False, None, f"android.jar not found at {jar_path}. Check the path."

def check_ndk_optional(ndk_path):
    """Optional: only validated if --ndk-path is given. The pipeline itself no longer builds anything."""
    if not ndk_path:
        return True, None, "Skipped (not needed: builds are done by Android Studio/Gradle)"

    ndk_dir = Path(ndk_path)
    if not ndk_dir.exists():
        return False, None, f"Provided NDK path does not exist: {ndk_path}"
    return True, str(ndk_dir.resolve()), f"Found at {ndk_dir}"

# --- Main Execution ---

def main():
    parser = argparse.ArgumentParser(description="Stratum Stage 00 - Setup and Validation")
    parser.add_argument("--jdk-path", type=str, help="Path to JDK 17+ (Optional if JAVA_HOME/javap is on PATH)")
    parser.add_argument("--sdk-path", type=str, help="Path to full Android SDK (Optional if --jar-path is used)")
    parser.add_argument("--jar-path", type=str, help="Direct path to android.jar (Bypasses --sdk-path)")
    parser.add_argument("--api-version", type=str, default="35", help="Target Android API version (Default: 35)")
    parser.add_argument("--ndk-path", type=str, help="(Optional) Android NDK path; only validated if given")
    parser.add_argument("--output", type=str, required=True, help="Output directory for setup_report.json")

    # Deprecated flags: still ACCEPTED (and ignored) so old command lines don't break.
    for old in ("--ndk-api", "--cmake-path", "--nanobind-version", "--python-target-version", "--py-version"):
        parser.add_argument(old, type=str, default=None, help=argparse.SUPPRESS)

    args = parser.parse_args()

    print("==================================================")
    print("      STRATUM PIPELINE - STAGE 00 (SETUP)         ")
    print("==================================================\n")

    errors = []

    py_ok, py_msg = check_python()
    print(f"[{'OK' if py_ok else 'FAIL'}] Python 3.10+   : {py_msg}")
    if not py_ok: errors.append(py_msg)

    javap_ok, javap_path, javap_msg = check_javap(args.jdk_path)
    print(f"[{'OK' if javap_ok else 'FAIL'}] Java (javap)   : {javap_msg}")
    if not javap_ok: errors.append(javap_msg)

    jar_ok, jar_path, jar_msg = check_android_jar(args.sdk_path, args.jar_path, args.api_version)
    print(f"[{'OK' if jar_ok else 'FAIL'}] Android API {args.api_version} : {jar_msg}")
    if not jar_ok: errors.append(jar_msg)

    ndk_ok, ndk_resolved_path, ndk_msg = check_ndk_optional(args.ndk_path)
    print(f"[{'OK' if ndk_ok else 'FAIL'}] Android NDK    : {ndk_msg}")
    if not ndk_ok: errors.append(ndk_msg)

    all_ok = all([py_ok, javap_ok, jar_ok, ndk_ok])

    report = {
        "all_ok": all_ok,
        "javap_path": javap_path,                      # used by Stage 03
        "javap_version": javap_msg if javap_ok else None,
        "jar_path": jar_path,                          # used by Stage 01
        "sdk_path": str(Path(args.sdk_path).resolve()) if args.sdk_path else None,
        "android_api": args.api_version,               # used by Stage 01
        "python_version": py_msg,
        "ndk_path": ndk_resolved_path,                 # optional, informational only
        "timestamp": datetime.now().isoformat()
    }

    out_dir = Path(args.output)
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