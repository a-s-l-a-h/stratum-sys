

# Stratum Pipeline — Stage 01: Extract Android JAR

## Overview & Architectural Context

Stage 01 serves as the foundational data source provider for the Stratum pipeline. It reads the configuration emitted by **Stage 00 (`00_setup`)**, locates the target `android.jar` SDK stub archive, and extracts a filtered, clean mirror of all compiled Java bytecode (`.class`) files into an on-disk directory classpath.

### In the Stratum Architecture:
- Downstream inspection (**Stage 02**) and reflection parsing (**Stage 03** via `javap`) require individual class files indexed directly on the filesystem rather than locked inside an opaque ZIP container.
- Stage 01 acts as a deterministic, cached mirror: non-class entries (such as `META-INF/` manifests, signatures, and resources) are filtered out at unpack time so subsequent stages only process pure Java type definitions.
- **Cross-Platform Resilience**: Fully supports Windows, Linux, and macOS. Paths can be relative or absolute, may contain spaces when enclosed in double quotes `""`, and `--setup` accepts either the direct JSON file or its parent directory.

---

## Quick Start / CLI Demo

### Recommended (Using Build Directory Paths)
```bash
python 01_extract/main.py \
    --setup "build/API_35/00_setup" \
    --output "build/API_35/01_extract"
```

### Passing the Direct JSON File Path
```bash
python 01_extract/main.py \
    --setup "build/API_35/00_setup/setup_report.json" \
    --output "build/API_35/01_extract"
```

### Paths with Spaces (Windows / Linux / macOS)
```powershell
python 01_extract/main.py ^
    --setup "D:/My Project/build/API_35/00_setup" ^
    --output "D:/My Project/build/API_35/01_extract"
```

### Force Re-extraction (Bypasses Cache)
```bash
python 01_extract/main.py \
    --setup "build/API_35/00_setup" \
    --output "build/API_35/01_extract" \
    --force
```

---

## Command-Line Arguments

| Flag | Type | Required | Description |
|---|---|:---:|---|
| `--setup` | `Path` | **Yes** | Path to `setup_report.json` **OR** the folder containing it (e.g., `"build/API_35/00_setup"`). Absolute or relative; supports spaces enclosed in quotes. |
| `--output` | `Path` | **Yes** | Destination directory where the extracted `.class` mirror and `extract_summary.json` will be stored. Automatically created if it does not exist. |
| `--force` | `Flag` | No | Overrides cache validation. Forces re-unpacking even if `extract_summary.json` is already present. |

---

## Execution Workflow

1. **Path Normalization & Setup Resolution**:
   - Strips any extraneous quotes or surrounding whitespace from CLI inputs.
   - Converts relative paths to canonical absolute paths (`Path.resolve()`).
   - If `--setup` points to a directory, it automatically resolves to `<setup_dir>/setup_report.json`.
   - Reads `jar_path` and `android_api` using UTF-8 encoding. Exits with code `1` if the report or `android.jar` does not exist.
2. **Cache Verification**:
   - Inspects `--output` for an existing `extract_summary.json`.
   - If found and `--force` is **not** specified, it emits a `[CACHE HIT]` message and exits immediately (`code 0`) to prevent redundant I/O.
   - If missing or `--force` is passed, extraction proceeds.
3. **Selective Extraction**:
   - Opens `android.jar` through Python’s streaming `zipfile.ZipFile`.
   - Filters out non-class entries and metadata (skipping all `META-INF/` files).
   - Extracts each valid class into `--output`, preserving original Java package directory trees (e.g., `android/widget/Button.class`).
   - Logs progress to the console every 2,000 extracted classes.
4. **Summary Serialization**:
   - Aggregates class package frequencies, sorts them descending by class count, and writes the summary to `extract_summary.json` using UTF-8 encoding.

---

## Output Structure

```text
build/API_35/01_extract/
├── extract_summary.json
└── android/
    ├── app/
    │   ├── Activity.class
    │   └── Service.class
    ├── view/
    │   ├── View.class
    │   └── ViewGroup.class
    ├── widget/
    │   ├── Button.class
    │   └── TextView.class
    └── ...
```

---

## Output Schema: `extract_summary.json`

```json
{
  "android_version": "35",
  "total_classes": 14823,
  "extraction_time_seconds": 3.42,
  "packages": {
    "android.view": 412,
    "android.content": 387,
    "android.widget": 341,
    "android.graphics": 298,
    "android.os": 276
  },
  "timestamp": "2026-03-26T22:10:05.123456"
}
```

### JSON Fields

- `android_version` (`string`): The target Android API version recorded by Stage 00.
- `total_classes` (`int`): Total count of non-metadata `.class` files unpacked.
- `extraction_time_seconds` (`float`): Total wall-clock time spent unzipping.
- `packages` (`object`): Map of Java package names to class counts, sorted from largest to smallest.
- `timestamp` (`string`): ISO-8601 execution timestamp.

---

## Troubleshooting & Exit Codes

| Exit Code | Reason | Resolution |
|:---:|---|---|
| `0` | Successful extraction or valid cache hit. | Proceed to **Stage 02 (`02_inspect`)**. |
| `1` | `setup_report.json` missing or cannot be resolved. | Verify that Stage 00 ran successfully and that `--setup` points to its output directory or JSON file. |
| `1` | `android.jar` missing at path specified in setup report. | Ensure the path passed to `--jar-path` during Stage 00 exists on disk and is readable. |
| `1` | `jar_path` key missing in `setup_report.json`. | Re-run Stage 00 to generate a fresh, uncorrupted setup report. |