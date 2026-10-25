
# Stratum Pipeline — Stage 03: Extract JNI Signatures via javap

## Overview & Architectural Context

Stage 03 executes the Java Class File Disassembler (`javap`) against every class selected in **Stage 02**. This step extracts internal JNI type signatures and constant values for all constructors, methods, and fields.

### In the Stratum Architecture:
- Android runtime reflection in C++ (`GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `NewObjectA`) requires exact JVM bytecode signatures (e.g., `(ILjava/lang/String;)V`).
- Stage 03 runs `javap` with:
  - `-s`: Exposes internal JNI type descriptors.
  - `-p`: Exposes all members (public, protected, package-private, and private).
  - `-constants`: Exposes static final constant values.
- The raw output files (`.javap`) preserve parameter types, exception throws, and constant field values, providing the complete raw syntax necessary for Stage 04's AST parser.
- **Cross-Platform & Flexible Resolution**: Supports arbitrary relative or absolute paths, handles spaces wrapped in quotes `""`, and accepts either direct JSON files or their parent directories for `--setup` and `--targets`.

---

## Quick Start / CLI Demo

### Recommended Pipeline Execution (Matching Build Layout)
```bash
python 03_javap/main.py \
    --input "build/API_35/01_extract" \
    --targets "02_inspect/targets.json" \
    --setup "build/API_35/00_setup" \
    --output "build/API_35/03_javap"
```

### Passing Folder Paths for Configs (Directory Fallback)
```bash
# Both --targets and --setup accept either the folder or the .json file directly
python 03_javap/main.py \
    --input "build/API_35/01_extract" \
    --targets "02_inspect" \
    --setup "build/API_35/00_setup" \
    --output "build/API_35/03_javap"
```

### Windows PowerShell (With Spaces in Paths)
```powershell
python 03_javap/main.py `
    --input "D:/My Project/build/API_35/01_extract" `
    --targets "02_inspect/targets.json" `
    --setup "D:/My Project/build/API_35/00_setup" `
    --output "D:/My Project/build/API_35/03_javap"
```

---

## Command-Line Arguments

| Flag | Type | Required | Description |
|---|---|:---:|---|
| `--input` | `Path` | **Yes** | Path to extracted `.class` files directory from Stage 01 (e.g., `"build/API_35/01_extract"`). Serves as the `-classpath` for `javap`. |
| `--targets` | `Path` | **Yes** | Path to `targets.json` **OR** its parent folder (e.g., `"02_inspect"`). Determines execution mode (`full` vs `manual`). |
| `--setup` | `Path` | **Yes** | Path to `setup_report.json` **OR** its parent folder (e.g., `"build/API_35/00_setup"`). Provides the verified JDK `javap` binary path. |
| `--output` | `Path` | **Yes** | Destination directory where individual `.javap` output files and `javap_summary.json` will be written. |

---

## Execution Workflow

1. **Path Normalization & Input Validation**:
   - Strips whitespace and outer quotes from all path arguments.
   - Resolves all paths to canonical absolute filesystem paths (`Path.resolve()`).
   - If `--setup` or `--targets` points to a directory, it automatically resolves to `setup_report.json` or `targets.json`.
   - Validates that `--input` directory, `targets.json`, and `setup_report.json` exist on disk.
2. **Environment & Tool Resolution**:
   - Reads `setup_report.json` using UTF-8 to obtain the verified JDK path for `javap`. If not configured, defaults to system `javap`.
3. **Target Filtering**:
   - Reads `targets.json` using UTF-8.
   - If `mode == "full"`: Scans `--input` recursively for all `.class` files (e.g., 5,981 classes in Android API 35) and alphabetizes them.
   - If `mode == "manual"`: Filters `targets` for classes where `"enabled": true`.
4. **Disassembly Subprocess Execution**:
   - For each target class, runs:
     ```bash
     javap -s -p -constants -classpath <input_dir> <fqn>
     ```
   - Sets a 30-second timeout per class to prevent hanging on corrupted bytecode.
5. **File Mirroring & Summary Report**:
   - Writes each disassembled class to `--output/<package_path>/<ClassName>.javap` with UTF-8 encoding.
   - Writes operational metrics to `javap_summary.json` detailing successful extractions and any failed classes with stderr output.

---

## Output Structure

```text
build/API_35/03_javap/
├── javap_summary.json
└── android/
    ├── app/
    │   ├── Activity.javap
    │   └── Service.javap
    ├── view/
    │   ├── View.javap
    │   └── ViewGroup.javap
    └── widget/
        ├── Button.javap
        └── TextView.javap
```

### Sample `.javap` Disassembly Output

```java
Compiled from "Button.java"
public class android.widget.Button extends android.widget.TextView {
  public android.widget.Button(android.content.Context);
    descriptor: (Landroid/content/Context;)V

  public android.widget.Button(android.content.Context, android.util.AttributeSet);
    descriptor: (Landroid/content/Context;Landroid/util/AttributeSet;)V

  public java.lang.CharSequence getAccessibilityClassName();
    descriptor: ()Ljava/lang/CharSequence;

  public android.widget.Button(android.content.Context, android.util.AttributeSet, int);
    descriptor: (Landroid/content/Context;Landroid/util/AttributeSet;I)V
}
```

---

## Summary Report Schema: `javap_summary.json`

```json
{
  "mode": "full",
  "total_processed": 5981,
  "successful": 5981,
  "failed": 0,
  "failed_classes": []
}
```

### Failed Class Format (if any errors occur)

```json
{
  "mode": "manual",
  "total_processed": 1,
  "successful": 0,
  "failed": 1,
  "failed_classes": [
    {
      "fqn": "android.widget.NonExistentView",
      "error": "Error: class not found: android.widget.NonExistentView"
    }
  ]
}
```

---

## Troubleshooting & Exit Codes

| Exit Code | Reason | Resolution |
|:---:|---|---|
| `0` | All target classes disassembled successfully (or non-fatal class errors logged). | Proceed to **Stage 04 (`04_parse`)**. |
| `1` | Missing inputs (`--input`, `--targets`, or `--setup` does not exist). | Ensure Stages 00, 01, and 02 have been executed. |
| `1` | `javap` binary executable not found or invalid. | Verify `javap_path` in `setup_report.json` or ensure Java is on `PATH`. |
| `1` | No classes to process. | Ensure `targets.json` has at least one class with `"enabled": true` or set `"mode": "full"`. |