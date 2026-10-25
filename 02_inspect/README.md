

# Stratum Pipeline — Stage 02: Inspect Classes & Target Configuration

## Overview & Architectural Context

Stage 02 acts as the boundary between raw SDK extraction and pipeline compilation configuration. It traverses the unpacked `.class` directory mirror produced by **Stage 01 (`01_extract`)**, catalogs all available Fully Qualified Names (FQNs), and generates two browsable reference lists (a flat alphabetical index and a package-tree breakdown).

Most importantly, Stage 02 verifies and preserves the primary target configuration file:
```
02_inspect/targets.json
```
This configuration dictates whether subsequent pipeline stages operate in **full API mode** (bridging all available Android classes) or **manual mode** (bridging a prioritized subset of classes). Because Stratum's C++ engine uses a unified runtime metadata registry rather than per-class C++ template instantiations, `"mode": "full"` can be enabled without risk of compiler Out-Of-Memory (OOM) failures.

### Key Architectural Guarantees:
- **CWD-Independent Discovery**: The script locates `targets.json` relative to its own script directory (`Path(__file__).parent`), meaning you can invoke it from the project root, a build subfolder, or a CI script without misplacing configuration files.
- **Developer Edit Preservation**: If `targets.json` already exists (e.g., configured to `"mode": "full"`), the file is **never modified or overwritten**.
- **Cross-Platform Resilience**: Handles Windows, macOS, and Linux paths natively, supports paths containing spaces, and strips redundant quotation marks.

---

## Quick Start / CLI Demo

### Recommended Pipeline Execution (Matching Build Layout)
```bash
python 02_inspect/main.py \
    --input "build/API_35/01_extract" \
    --output "build/API_35/02_inspect"
```

### Windows PowerShell (With Spaces in Paths)
```powershell
python 02_inspect/main.py `
    --input "D:/My Project/build/API_35/01_extract" `
    --output "D:/My Project/build/API_35/02_inspect"
```

### Specifying an Explicit Targets File Location (Optional)
```bash
python 02_inspect/main.py \
    --input "build/API_35/01_extract" \
    --output "build/API_35/02_inspect" \
    --targets "02_inspect/targets.json"
```

---

## Command-Line Arguments

| Flag | Type | Required | Description |
|---|---|:---:|---|
| `--input` | `Path` | **Yes** | Root directory containing extracted `.class` files from Stage 01 (e.g., `"build/API_35/01_extract"`). Absolute or relative; supports spaces in quotes. |
| `--output` | `Path` | **Yes** | Destination directory where `available_classes.txt` and `available_by_package.txt` will be written. Automatically created if missing. |
| `--targets` | `Path` | No | Custom path to `targets.json`. If omitted, defaults safely to `02_inspect/targets.json` adjacent to the script. |

---

## Execution Workflow

1. **Path Resolution & Normalization**:
   - Strips whitespace and outer quotes from `--input`, `--output`, and `--targets`.
   - Resolves all paths to absolute filesystem locations (`Path.resolve()`).
   - Validates that `--input` exists and is a valid directory.
2. **Input Scan**:
   - Recursively traverses `--input` for all compiled Java bytecode files ending in `.class`.
   - Exits with an error if no classes are discovered.
3. **Cross-Platform FQN Normalization**:
   - Computes relative paths from `--input` using `.as_posix()` to normalize separators (`/` vs `\`).
   - Strips `.class` and maps directory slashes to standard Java package dots (e.g., `android/widget/Button.class` $\rightarrow$ `android.widget.Button`).
   - Indexes names into an alphabetical flat list and a package-to-class dictionary.
4. **Artifact Generation**:
   - Writes `available_classes.txt`: A single-column, sorted list of every available class FQN.
   - Writes `available_by_package.txt`: A package-grouped breakdown displaying class densities per package.
5. **Configuration Handling (`targets.json`)**:
   - Evaluates the target configuration path (defaulting to `02_inspect/targets.json`).
   - **If present**: Emits a `[SAFE]` notice and leaves existing contents (e.g., `"mode": "full"`) completely untouched.
   - **If missing**: Generates a default starter configuration containing standard Android core classes.

---

## Output Structure

```text
stratum-sys/
├── 02_inspect/
│   ├── main.py
│   └── targets.json                   <-- Developer config (Preserved across runs)
│
└── build/API_35/02_inspect/           <-- Build-specific artifacts
    ├── available_classes.txt          <-- Alphabetical flat list of all FQNs
    └── available_by_package.txt       <-- Package-grouped index with class counts
```

---

## Target Configuration Reference (`targets.json`)

### 1. Full Mode (Recommended for Complete SDK Generation)
Processes every `.class` file extracted in Stage 01. The `targets` list is bypassed:
```json
{
  "mode": "full",
  "targets": []
}
```

### 2. Manual Mode (Selective / Targeted Compilation)
Processes only classes explicitly registered with `"enabled": true`:
```json
{
  "mode": "manual",
  "targets": [
    {
      "fqn": "android.app.Activity",
      "enabled": true,
      "priority": 1,
      "notes": "core lifecycle"
    },
    {
      "fqn": "android.view.View",
      "enabled": true,
      "priority": 1,
      "notes": "base UI component"
    },
    {
      "fqn": "android.widget.DatePicker",
      "enabled": false,
      "priority": 3,
      "notes": "disabled for current test run"
    }
  ]
}
```

### Operational Modes (`mode`)

| Mode | Scope | Description |
|---|---|---|
| `"full"` | **All Classes** | Instructs Stage 03 to process every single `.class` file cataloged in Stage 01. The `targets` array is ignored. |
| `"manual"` | **Selective** | Stage 03 only decompiles classes in `targets` where `"enabled": true`. Ideal for rapid iteration or targeted testing. |

---

## Troubleshooting & Exit Codes

| Exit Code | Reason | Resolution |
|:---:|---|---|
| `0` | Success. Class catalogs generated and `targets.json` confirmed. | Proceed to **Stage 03 (`03_javap`)**. |
| `1` | `--input` directory does not exist or contains 0 `.class` files. | Run Stage 01 (`01_extract`) first or verify `--input` points to the extracted SDK folder. |
| `1` | Write permissions denied on `--output` or `--targets` location. | Check filesystem permissions for the target directory. |