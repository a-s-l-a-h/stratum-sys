
# Stratum Pipeline — Stage 00: Setup & Environment Validation

## Overview

**Stage 00 (`00_setup/main.py`)** is the lightweight pre-flight gatekeeper of the Stratum pipeline. It validates your host environment, ensures the required tools and assets (`python`, `javap`, `android.jar`, and optional metadata files like `api-versions.xml` and `hiddenapi-flags.csv`) are reachable and operational, and generates `setup_report.json`.

Downstream stages read `setup_report.json` as their single source of truth. You configure your paths once in Stage 00; subsequent stages consume the report automatically.

```
       ┌──────────────────────────────────────────────────────────┐
       │                   Stage 00 (main.py)                     │
       └─────────────────────────────┬────────────────────────────┘
                                     │ Generates
                                     ▼
                     <output_dir>/setup_report.json
                                     │
         ┌───────────────────────────┼───────────────────────────┐
         ▼                           ▼                           ▼
   Stage 01 (Extract)         Stage 04.5 (Sanitizer)       Stage 03 (Javap)
  • jar_path                  • api_versions_xml_path     • javap_path
  • android_api               • hiddenapi_path            • javap_version
```

---

## What This Stage Does

1. **Python Environment Verification**: Ensures host Python is 3.10+ (required for modern union typing and pipeline features).
2. **Java Disassembler (`javap`) Resolution**:
   - Accepts a direct path to `javap` / `javap.exe`, a JDK root folder, a `bin/` folder, or falls back to system `PATH`.
   - Executes `javap -version` to verify that Java is functioning properly.
   - **Optional Version Check**: If `--jdk-version` is passed (e.g. `17`), it verifies that the JDK matches the specified major version.
3. **Android Platform Assets & Metadata Verification**:
   - Confirms that the target `android.jar` exists and resolves it to a canonical absolute path.
   - (Optional) Verifies and resolves the path to `api-versions.xml` if provided.
   - (Optional) Verifies and resolves the path to `hiddenapi-flags.csv` if provided.
4. **State File Generation**:
   - Emits `setup_report.json` with resolved absolute paths to your specified `--output` folder.

---

## Prerequisites & Offline Metadata

| Requirement | Minimum | Purpose | How to Provide |
|---|---|---|---|
| **Python** | `3.10+` | Host pipeline runner | Host pre-installed |
| **JDK (`javap`)** | Any working version | Bytecode parsing in Stage 03 | System `PATH` or `--javap-path` |
| **android.jar** | Target API (e.g., 35) | Stub class definitions | Any local path via `--jar-path` |
| **api-versions.xml** | Matching Target API | SDK version history (`since`/`deprecated`/`removed`) | Optional via `--api-versions-xml-path` |
| **hiddenapi-flags.csv** | Matching Target API | AOSP non-SDK/hidden API access flags | Optional via `--hiddenapi-path` |

---

## Where to Find Metadata Files (`api-versions.xml` & `hiddenapi-flags.csv`)

The Stratum pipeline operates **100% offline**. You can store these metadata files inside your repository (e.g., `third_party/`) or reference them directly from your local Android SDK.

```text
third_party/
├── android-35.jar
├── api_versions/
│   └── 35/
│       └── api-versions.xml        <-- SDK Version History
└── hiddenapi/
    └── 35/
        └── hiddenapi-flags.csv     <-- AOSP Hidden API Access Flags
```

### 1. `api-versions.xml` (Android SDK Version History)

Tells the pipeline which Android API level introduced (`since`), deprecated (`deprecated`), or deleted (`removed`) every class, method, and field.

* **Option A: Copy from your Local Android SDK (Fastest & Matching)**
  If Android Studio or the Android SDK is installed, this file is already on your machine:
  * **Windows:**
    ```cmd
    copy "%LOCALAPPDATA%\Android\Sdk\platforms\android-35\data\api-versions.xml" "third_party\api_versions\35\api-versions.xml"
    ```
  * **macOS:**
    ```bash
    mkdir -p third_party/api_versions/35
    cp ~/Library/Android/sdk/platforms/android-35/data/api-versions.xml third_party/api_versions/35/api-versions.xml
    ```
  * **Linux:**
    ```bash
    mkdir -p third_party/api_versions/35
    cp ~/Android/Sdk/platforms/android-35/data/api-versions.xml third_party/api_versions/35/api-versions.xml
    ```

* **Option B: Download from Official AOSP Git**
  * **AOSP Webview:** [platform/development/+/refs/heads/main/sdk/api-versions.xml](https://android.googlesource.com/platform/development/+/refs/heads/main/sdk/api-versions.xml)
  * **Direct Raw Download (API 35 / Android 15):**
    ```bash
    curl -Lo third_party/api_versions/35/api-versions.xml \
      "https://android.googlesource.com/platform/development/+/refs/heads/android15-release/sdk/api-versions.xml?format=TEXT"
    ```
    *(Note: Responses with `?format=TEXT` are base64-encoded. If using `curl`, decode with `base64 --decode`).*

---

### 2. `hiddenapi-flags.csv` (AOSP Hidden API Access Flags)

Contains the non-SDK interface restrictions (`blocked`, `unsupported`, `max-target-*`, `public-api`) used by Stage 04.5 to sanitize members and prevent runtime crashes.

* **Option A: Official Google Documentation & Developer Guides**
  * [Android Developer Guide: Restrictions on non-SDK interfaces](https://developer.android.com/guide/app-compatibility/restrictions-non-sdk-interfaces)
  * [Android 15 Non-SDK Interface Changes](https://developer.android.com/about/versions/15/changes/non-sdk-15)

* **Option B: Official AOSP Build Artifacts & Prebuilts**
  In full AOSP builds, this file is generated at `out/soong/hiddenapi/hiddenapi-flags.csv`. Prebuilt artifacts can be found in the Android Runtime repository:
  * **AOSP Runtime Prebuilts:** [platform/prebuilts/runtime/+/refs/heads/main/hiddenapi/](https://android.googlesource.com/platform/prebuilts/runtime/+/refs/heads/main/hiddenapi/)
  * **Android 15 Branch:** [platform/frameworks/base/+/refs/heads/android15-release](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release)

* **Option C: Pre-extracted Community Mirrors**
  Open-source compatibility projects maintain direct, pre-extracted CSVs:
  * [AOSP HiddenApi Dumps on GitHub](https://github.com/anggrayudi/android-hidden-api)

---

## Command-Line Reference

```bash
python 00_setup/main.py --jar-path <PATH> --output <PATH> [OPTIONS]
```

### Required Arguments

| Argument | Type | Description |
|---|---|---|
| `--jar-path` | `PATH` | Path to `android.jar` (any relative or absolute path, e.g., `third_party/android-35.jar`). |
| `--output` | `PATH` | Output directory where `setup_report.json` will be written (e.g., `build/API_35/00_setup`). |

### Optional Configuration

| Argument | Type | Default | Description |
|---|---|---|---|
| `--javap-path` / `--jdk-path` | `PATH` | System `PATH` | Path to `javap` executable **OR** JDK root folder (automatically checks `bin/`). |
| `--jdk-version` | `STRING` | `None` | Optional version cross-check for the detected JDK (e.g., `"17"`, `"21"`). |
| `--api-version` | `STRING` | `"35"` | Target Android API version label (Default: `"35"`). |
| `--api-versions-xml-path` | `PATH` | `None` | Direct path to `api-versions.xml` (Optional). |
| `--hiddenapi-path` | `PATH` | `None` | Direct path to `hiddenapi-flags.csv` (Optional). |

*(Note: Legacy arguments like `--sdk-path`, `--ndk-path`, and `--cmake-path` are silently ignored so legacy build scripts do not break).*

---

## Usage Examples

> **Best Practice**: Always enclose paths and arguments in double quotes `""` to prevent spaces or special characters from breaking the command.

### Example 1: Full Setup (With JDK, API XML, and Hidden API flags)

```powershell
# Windows PowerShell
python 00_setup/main.py `
  --jar-path "third_party/android-35.jar" `
  --api-version "35" `
  --javap-path "C:/Program Files/Java/jdk-17" `
  --jdk-version "17" `
  --api-versions-xml-path "third_party/api_versions/35/api-versions.xml" `
  --hiddenapi-path "third_party/hiddenapi/35/hiddenapi-flags.csv" `
  --output "build/API_35/00_setup"
```

```bash
# Linux / macOS
python3 00_setup/main.py \
  --jar-path "third_party/android-35.jar" \
  --api-version "35" \
  --javap-path "/usr/lib/jvm/java-17-openjdk" \
  --jdk-version "17" \
  --api-versions-xml-path "third_party/api_versions/35/api-versions.xml" \
  --hiddenapi-path "third_party/hiddenapi/35/hiddenapi-flags.csv" \
  --output "build/API_35/00_setup"
```

### Example 2: Minimal Run (Using System `PATH` for Java)

If `javap` is already in your system `PATH` and you only need core setup:

```bash
python 00_setup/main.py \
  --jar-path "third_party/android-35.jar" \
  --api-version "35" \
  --output "build/API_35/00_setup"
```

---

## Output: `setup_report.json`

On success, `setup_report.json` is generated inside the `--output` folder with all supplied paths resolved to canonical absolute paths:

```json
{
  "all_ok": true,
  "javap_path": "C:\\Program Files\\Java\\jdk-17\\bin\\javap.exe",
  "javap_version": "Found v17.0.10 at C:\\Program Files\\Java\\jdk-17\\bin\\javap.exe",
  "jdk_version": "17",
  "jar_path": "D:\\android-35.jar",
  "api_versions_xml_path": "E:\\api_versions\\35\\api-versions.xml",
  "hiddenapi_path": "E:\\hiddenapi\\35\\hiddenapi-flags.csv",
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
| `android_api` | **Stage 01**, **Stage 04.5** | Tracks the compilation target API level. |
| `api_versions_xml_path` | **Stage 04.5 (`04_5_sanitize`)** | Parses min/introduced/deprecated SDK API level data. |
| `hiddenapi_path` | **Stage 04.5 (`04_5_sanitize`)** | Identifies blacklisted/greylisted/hidden members. |
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

### 4. `api-versions.xml not found at ...` or `hiddenapi-flags not found at ...`
- **Cause**: You provided `--api-versions-xml-path` or `--hiddenapi-path`, but the target file does not exist at that location.
- **Solution**: Double check for typos or omit the flags if you do not intend to supply them.

---

## Next Step in the Pipeline

Once Stage 00 completes with `[OK]`, proceed to **Stage 01: Extract**:

```bash
python 01_extract/main.py --setup "build/API_35/00_setup/setup_report.json" --output "build/API_35/01_extract"
```