
# Stratum Pipeline — Stage 00: Setup & Environment Validation

## Overview

**Stage 00 (`00_setup/main.py`)** is the lightweight pre-flight gatekeeper of the Stratum pipeline. It validates your host environment, ensures the required tools (`python`, `javap`, and `android.jar`) are reachable and operational, and generates `setup_report.json`.

Downstream stages read `setup_report.json` as their single source of truth. You configure your paths once in Stage 00; subsequent stages consume the report automatically.

```
       ┌──────────────────────────────────────────────────────────┐
       │                   Stage 00 (main.py)                     │
       └─────────────────────────────┬────────────────────────────┘
                                     │ Generates
                                     ▼
                     <output_dir>/setup_report.json
                                     │
         ┌───────────────────────────┴───────────────────────────┐
         ▼                                                       ▼
   Stage 01 (Extract)                                      Stage 03 (Javap)
  • jar_path                                              • javap_path
  • android_api                                           • javap_version
```

---

## What This Stage Does

1. **Python Environment Verification**: Ensures host Python is 3.10+ (required for modern union typing and pipeline features).
2. **Java Disassembler (`javap`) Resolution**:
   - Accepts a direct path to `javap` / `javap.exe`, a JDK root folder, a `bin/` folder, or falls back to system `PATH`.
   - Executes `javap -version` to verify that Java is functioning properly.
   - **Optional Version Check**: If `--jdk-version` is passed (e.g. `17`), it verifies that the JDK matches the specified major version.
3. **Android Platform JAR Check**:
   - Confirms that the target `android.jar` exists and resolves it to a full canonical absolute path.
4. **State File Generation**:
   - Emits `setup_report.json` to your specified `--output` folder.

---

## Prerequisites

| Requirement | Minimum | Purpose | How to Provide |
|---|---|---|---|
| **Python** | `3.10+` | Host pipeline runner | Host pre-installed |
| **JDK (`javap`)** | Any working version | Bytecode parsing in Stage 03 | System `PATH` or `--javap-path` |
| **android.jar** | Target API (e.g., 35) | Stub class definitions | Any local path via `--jar-path` |

---

## Command-Line Reference

```bash
python 00_setup/main.py --jar-path <PATH> --output <PATH> [OPTIONS]
```

### Required Arguments

| Argument | Type | Description |
|---|---|---|
| `--jar-path` | `PATH` | Path to `android.jar` (any relative or absolute path, e.g., `third_party/android-35.jar` or from an Android SDK platform folder). |
| `--output` | `PATH` | Output directory where `setup_report.json` will be written (e.g., `build/API_35/00_setup`). |

### Optional Configuration

| Argument | Type | Default | Description |
|---|---|---|---|
| `--javap-path` / `--jdk-path` | `PATH` | System `PATH` | Path to the `javap` executable **OR** the JDK root folder (automatically checks `bin/`). |
| `--jdk-version` | `STRING` | `None` | (Optional) Cross-checks that the detected JDK matches a specific version (e.g., `"17"`, `"21"`). |
| `--api-version` | `STRING` | `"35"` | Target Android API version label (e.g., `"35"`). |

*(Note: Legacy arguments like `--sdk-path`, `--ndk-path`, and `--cmake-path` are silently ignored so legacy build scripts do not break).*

---

## Usage Examples

> **Best Practice**: Always enclose paths and arguments in double quotes `""` to prevent spaces or special characters from breaking the command.

### Example 1: Standard Run (Using JDK root folder)

```bash
# Windows PowerShell
python 00_setup/main.py `
  --jar-path "third_party/android-35.jar" `
  --api-version "35" `
  --javap-path "C:/Program Files/Java/jdk-17" `
  --output "build/API_35/00_setup"

# Linux / macOS
python3 00_setup/main.py \
  --jar-path "third_party/android-35.jar" \
  --api-version "35" \
  --javap-path "/usr/lib/jvm/java-17-openjdk" \
  --output "build/API_35/00_setup"
```

### Example 2: Strict JDK Version Check

If you want to ensure that only a specific JDK version (e.g. Java 17) is used:

```bash
python 00_setup/main.py `
  --jar-path "third_party/android-35.jar" `
  --api-version "35" `
  --javap-path "C:/Program Files/Java/jdk-17" `
  --jdk-version "17" `
  --output "build/API_35/00_setup"
```

### Example 3: Relying on System `PATH`

If `javap` is already available on your system `PATH`:

```bash
python 00_setup/main.py `
  --jar-path "third_party/android-35.jar" `
  --api-version "35" `
  --output "build/API_35/00_setup"
```

---

## Output: `setup_report.json`

On success, `setup_report.json` is generated inside the `--output` folder with all paths resolved into absolute paths:

```json
{
  "all_ok": true,
  "javap_path": "C:\\Program Files\\Java\\jdk-17\\bin\\javap.exe",
  "javap_version": "Found v17.0.10 at C:\\Program Files\\Java\\jdk-17\\bin\\javap.exe",
  "jar_path": "C:\\Projects\\stratum\\third_party\\android-35.jar",
  "sdk_path": null,
  "android_api": "35",
  "python_version": "Found v3.11.0",
  "ndk_path": null,
  "timestamp": "2026-10-25T11:58:05.380030"
}
```

### Downstream Pipeline Consumption

| Field | Consumed By | Purpose |
|---|---|---|
| `all_ok` | All Stages | Pre-flight validation gatekeeper (`true` required). |
| `jar_path` | **Stage 01 (`01_extract`)** | Path to the `.jar` unpacked to extract class metadata. |
| `android_api` | **Stage 01**, **Stage 02** | Tracks the compilation target API level. |
| `javap_path` | **Stage 03 (`03_javap`)** | Invoked to parse Java bytecode and method signatures. |

---

## Troubleshooting Guide

### 1. `javap executable not found`
- **Cause**: Java is not in your system `PATH`, or the path provided to `--javap-path` is invalid.
- **Solution**: Pass the direct JDK directory or binary using `--javap-path "path/to/jdk"`.

### 2. `Found v..., but does not match expected version '17'`
- **Cause**: `--jdk-version "17"` was passed, but the discovered `javap` is a different version (e.g. Java 11 or Java 21).
- **Solution**: Point `--javap-path` to the correct JDK folder or adjust `--jdk-version`.

### 3. `android.jar not found at ...`
- **Cause**: The path passed to `--jar-path` does not exist or the file is unreadable.
- **Solution**: Verify the file path. Relative paths are resolved against the directory where you run the terminal command.

---

## Next Step in the Pipeline

Once Stage 00 completes with `[OK]`, proceed to **Stage 01: Extract**:

```bash
python 01_extract/main.py --setup "build/API_35/00_setup/setup_report.json" --output "build/API_35/01_extract"
```
