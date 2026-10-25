
> This repo in development so This repository is undergoing frequent changes.
> This repo on based on stratum . but in this repo stratum to focus on c target and cpp target insted of python .
<div align="center">



# For python target( nanobind based ) visit this link 

https://github.com/a-s-l-a-h/stratum

# 🌉 Stratum-sys

---
### Automated 

configure pipeline.json 

Then run 

python run_pipeline_helper.py --list        # check the config loads, shows jobs and stage toggles
python run_pipeline_helper.py --dry-run     # prints every command without downloading or running anything
python run_pipeline_helper.py               # full run for all enabled jobs
python run_pipeline_helper.py --job 35      # just API 35

---

---
### Manual step by step

# Run commands for quick look

```
python 00_setup/main.py --jar-path "third_party/android-35.jar" --api-version "35" --javap-path "C:/Program Files/Eclipse Adoptium/jdk-17.0.20.101-hotspot" --jdk-version "17" --api-versions-xml-path "third_party/api_versions/35/api-versions.xml" --hiddenapi-path "third_party/hiddenapi/35/hiddenapi-flags.csv" --output "build/API_35/00_setup"
```

```
python 01_extract/main.py --setup "build/API_35/00_setup" --output "build/API_35/01_extract"
```

```
python 02_inspect/main.py --input "build/API_35/01_extract" --output "build/API_35/02_inspect"
```

```
python 03_javap/main.py --input "build/API_35/01_extract" --targets "02_inspect/targets.json" --setup "build/API_35/00_setup/setup_report.json" --output "build/API_35/03_javap"
```

```
python 04_parse/main.py --input "build/API_35/03_javap" --output "build/API_35/04_parse"
```

```
python 04_5_sanitize/main.py --input "build/API_35/04_parse" --output "build/API_35/04_5_sanitize" --strict
```
The xml and csv paths come from `build/API_35/00_setup/setup_report.json`, found automatically. Optional extras: `--min-sdk N`, `--setup "build/API_35/00_setup"`.

### Stage 05 (Pass 1)
```
python 05_resolve/main.py --input "build/API_35/04_5_sanitize" --output "build/API_35/05_resolve/output"
```

### Stage 05.5
```
python 05_5_abstract/main.py --input "build/API_35/05_resolve/output" --output "build/API_35/05_5_abstract/output" --mode on
```
Copy `build/API_35/05_5_abstract/output/java/com/stratum/adapters/*.java` into your Android Studio runtime module.

refer this to find where to place the adapter java files 

https://github.com/a-s-l-a-h/stratum_android_py_3_14

### Stage 05 (Pass 2)
```
python 05_resolve/main.py --input "build/API_35/05_5_abstract/output/patched" --output "build/API_35/05_resolve/output_patched"
```

### Stage 20: engine metadata
```
python 20_engine_core/emit.py --input "build/API_35/05_resolve/output_patched" --output "build/API_35/20_engine_core/generated"
```

### Stage 21: C++ SDK headers
```
python 21_cpp_emit/main.py --input "build/API_35/05_resolve/output_patched" --output "build/API_35/21_cpp_emit/output"
```
Optional: add `--emit-prefix "android.app,android.view"` to emit only some packages.

### Stage 21b: package
```
python 21_cpp_emit/package.py --input "build/API_35/21_cpp_emit/output" --generated "build/API_35/20_engine_core/generated" --adapters "build/API_35/05_5_abstract/output/java/com/stratum/adapters" --output "build/API_35/21_cpp_emit/output/stratum"
```
Copy the resulting `stratum/` folder to `app/src/main/cpp/stratum/` in Android Studio and build there.

---









# Run commands (OLD) for quick look
---

```
python 00_setup/main.py --jar-path third_party/android-35.jar --api-version 35 --output 00_setup/output

```
---

---

```
python 01_extract/main.py --setup 00_setup/output/setup_report.json --output 01_extract/output
```
---

---

```
python 02_inspect/main.py --input 01_extract/output --output 02_inspect/output
```

---

```
python 03_javap/main.py --input 01_extract/output --targets 02_inspect/targets.json --setup 00_setup/output/setup_report.json --output 03_javap/output
```
---

---

```
python 04_parse/main.py --input 03_javap/output --output 04_parse/output
```

---

---

```
python 04_5_sanitize/main.py --input 04_parse/output --output 04_5_sanitize/output --api-version 35 --strict
```
---

#### (add --strict to also drop restricted/unsupported/conditional hiddenapi members)
#### (add --min-sdk N to also drop members removed at/before API N)

---

### Stage 05 (Pass 1): Initial Resolution
### WITH Stage 04.5:

---

```
python 05_resolve/main.py --input 04_5_sanitize/output --output 05_resolve/output
```
---

### Stage 05.5: Abstract & Interface Adapters Generation 

---

```
python 05_5_abstract/main.py --input 05_resolve/output --output 05_5_abstract/output --mode on
```

---

#### -> Copy 05_5_abstract/output/java/com/stratum/adapters/*.java into Android Studio: runtime module . look the demo project . 

--- 
refer this to find where to place the adapter java files 

https://github.com/a-s-l-a-h/stratum_android_py_3_14

---

### Stage 05 (Pass 2): Final Resolution with Patched Callback Metadata 

---

```
python 05_resolve/main.py --input 05_5_abstract/output/patched --output 05_resolve/output_patched
```

---


---

new c abi target 


# 0. shared Java edits (section 2), then upstream exactly as today:
python 05_resolve/main.py --input 04_parse/output --output 05_resolve/output
python 05_5_abstract/main.py --input 05_resolve/output --output 05_5_abstract/output --mode on
python 05_resolve/main.py --input 05_5_abstract/output/patched --output 05_resolve/output_patched

# 1. engine
python 20_engine_core/emit.py  --input 05_resolve/output_patched
python 20_engine_core/build.py --abi arm64-v8a            # add --log for deep tracing; repeat for x86_64
# -> 20_engine_core/output/arm64-v8a/libstratum.so

# 2. SDK (start restricted; drop --emit-prefix later for the whole SDK)
python 21_cpp_emit/main.py --input 05_resolve/output_patched

python 21_cpp_emit/package.py  # when this run in output folder in stratum there is folder create stratum inside it all c and cpp code for stratum and cmake also there so you can copy and place in the android studio project and build there .

---



---
# stratum embed fully to single .so file it's depending py files all inside to single .so 

### this need same as 00 to 05_5 then 08  ,, and below 10 (10 emit , 10 build) choose static maybe see some perfomance improvment , and also choose --no-log in build when production time

```
python 08_pyi_emit/main.py --input 05_resolve/output_patched --output 08_pyi_emit/output --mode static 

```
```
python 10_embed/emit.py --input 05_resolve/output_patched --static-py 08_pyi_emit/output --setup 00_setup/output/setup_report.json --output 10_embed/output --mode static 
```
```
python 10_embed/build.py --core 10_embed/output/core --setup 00_setup/output/setup_report.json --nanobind third_party/nanobind --abi arm64-v8a --output 10_embed/output --no-log
``` 
---



---


# Project Directory Structure
```
stratum/
├── .gitignore
├── LICENSE
├── README.md
├── PYTHON_API.md
├── THIRD-PARTY-LICENSES.md
│
├── 00_setup/                               # Stage 00: Environment & Tooling Verification
│   ├── README.md
│   ├── main.py
│   └── output/                             # [Generated]
│       └── setup_report.json               # Environment verification report & paths
│
├── 01_extract/                             # Stage 01: Android JAR Extraction
│   ├── README.md
│   ├── main.py
│   └── output/                             # [Generated]
│       ├── android/                        # Mirrored extracted .class files
│       │   └── ...
│       └── extract_summary.json
│
├── 02_inspect/                             # Stage 02: Class Inspection & Target Configuration
│   ├── README.md
│   ├── main.py
│   ├── targets.json                        # [Tracked] Pipeline targets & inclusion mode
│   ├── target_jsons_pool/                  # Preset target configurations
│   └── output/                             # [Generated]
│       ├── available_classes.txt           # Flat list of all available Android FQNs
│       └── available_by_package.txt        # Classes grouped by package hierarchy
│
├── 03_javap/                               # Stage 03: Javap Bytecode Extraction
│   ├── README.md
│   ├── main.py
│   └── output/                             # [Generated]
│       ├── android/                        # Extracted .javap signatures per class
│       │   └── ...
│       └── javap_summary.json
│
├── 04_parse/                               # Stage 04: Signature & AST Parser
│   ├── README.md
│   ├── main.py
│   └── output/                             # [Generated]
│       ├── android/                        # Parsed class JSON representations
│       │   └── ...
│       └── parse_summary.json
│
├── 04_5_sanitize/                          # Stage 04.5: Optional JNI Safety & Hidden API Filter
│   ├── README.md
│   ├── main.py
│   └── output/                             # [Generated]
│       ├── android/                        # Stripped/sanitized class JSONs
│       └── sanitize_summary.json
│
├── 05_resolve/                             # Stage 05: Slot Assignment & Type Resolution (Pass 1 & Pass 2)
│   ├── README.md
│   ├── main.py
│   ├── targets.json                        # [Tracked] Closure & filter configuration
│   ├── target_jsons_pool/
│   ├── output/                             # [Generated - Pass 1] Used by Stage 05.5
│   │   ├── android/
│   │   └── resolve_summary.json
│   └── output_patched/                     # [Generated - Pass 2] Feeds Stage 06 and Stage 08
│       ├── android/
│       └── resolve_summary.json
│
├── 05_5_abstract/                          # Stage 05.5: Abstract Class & Multi-Method Adapter Generator
│   ├── README.md
│   ├── main.py
│   ├── targets.json                        # [Tracked] Abstract callback seeds & avoid list
│   └── output/                             # [Generated]
│       ├── java/
│       │   └── com/stratum/adapters/       # Generated Java adapter source files (.java)
│       │       └── Adapter_*.java
│       ├── patched/                        # Patched JSONs containing adapter metadata
│       └── manifest.json
│
update happen new folders came


│
├── runtime/                                # Runtime Java files (packaged into your Android app)
│   ├── README.md
│   └── java/
│       └── com/stratum/runtime/
│           ├── StratumActivity.java        # Android Activity managing Python lifecycle
│           └── StratumInvocationHandler.java # Generic Java Proxy dispatch handler
│
└── third_party/                            # External Dependencies & Offline Metadata Caches
    │
    │
    ├── api_versions/                       # [Expected / User Placed - NOT in git]
    │   └── <api-level>/                    # e.g., 35/
    │       └── api-versions.xml            # Copy from: <Android-SDK>/platforms/android-<api>/data/api-versions.xml
    │                                       # (Optional: Used by Stage 04.5 to tag sdk_since/deprecated/removed)
    │
    └── hiddenapi/                          # [Expected / User Placed - NOT in git]
        └── <api-level>/                    # e.g., 35/
            └── hiddenapi-flags.csv         # Placed manually from AOSP sources
                                            # (Optional: Used by Stage 04.5 to strip restricted/blocked APIs)

```
---------------------------------------------------

