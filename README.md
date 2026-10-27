# Stratum-Sys

Stratum-sys is an offline code generation pipeline and lightweight native runtime for Android. It introspects `android.jar`, extracts bytecode signatures, and generates:

1. **A standalone C engine (`libstratum.so`)** exposing a stable, pure C ABI (`stratum_c_abi.h`).
2. **A header-only C++17 SDK** that wraps Android platform APIs into RAII-managed, type-safe classes.
3. **An Android library module (`stratum-runtime`)** containing JVM lifecycle hooks and compiled Java adapter classes for abstract callbacks.

Instead of writing custom JNI glue for every native-to-Java interaction, native code calls generated C++ classes (or the underlying C ABI) directly.

---

## 💡 What Problem Does This Solve?

### 1. The "N-Bridge Hell" of Android Native Development
When building high-performance Android applications containing multiple native engines (compute, physics, signal processing, audio, or custom core logic), traditional development forces you into a fragmented architecture:
* Every native library requires its own manual JNI bridge or cumbersome Java wrapper to talk to the Java UI.
* Buffers and data arrays are repeatedly copied back and forth across the JNI boundary between the Java and native heaps, increasing latency and draining battery.
* Developers waste hundreds of hours maintaining raw JNI boilerplate (`FindClass`, `GetMethodID`, signature strings like `"([BII)V"`), managing local/global references, and debugging memory synchronization issues.

```text
          ┌────────────────────────────────────────┐
          │        Android UI / App (Java)         │
          └───┬────────────────┬───────────────┬───┘
              │ (JNI Bridge 1) │ (JNI Bridge 2)│ (JNI Bridge 3)
              ▼                ▼               ▼
      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
      │ Native Lib A │ │ Native Lib B │ │ Native Lib C │
      │   (C / C++)  │ │   (C / C++)  │ │   (C / C++)  │
      └──────────────┘ └──────────────┘ └──────────────┘
```

---

### 2. The Solution: The "Inverted Bridge" Architecture
Traditionally, developers must hand-write separate JNI glue code for every native library to expose it to Java.

**Stratum-sys inverts this model:** instead of bridging each native library up to Java, Stratum automatically wraps the entire Android platform (`android.jar`) into native headers. You get **one universal, auto-generated bridge**, allowing your native code to control Android directly without writing a single line of raw JNI:

```text
            ┌────────────────────────────────────────┐
            │        Android OS (Platform/Sensors)   │
            └───────────────────▲────────────────────┘
                                │ (Single Stratum Bridge)
    ════════════════════════════╪═════════════════════════════
    THE NATIVE ENGINE           │
                                ▼
       ┌──────────────────────────────────────────────────┐
       │ Hardware / Sensor Buffer (Native)                │
       │                         │ (Direct memory pointer)│
       │                         ▼                        │
       │                     Native Processing (C / C++)  │
       │                         │ (Direct memory pointer)│
       │                         ▼                        │
       │                     Custom Core Engine (C / C++) │
       └──────────────────────────────────────────────────┘
```

---

### 3. What Stratum-sys Delivers

* **🛡️ Complete JNI Eradication:**  
  Never write `JNIEnv*`, `FindClass`, method signatures, or manual local-reference management in application logic. Stratum turns the Android SDK into clean, type-safe C++17 classes:
  ```cpp
  // No raw JNI — pure, type-safe C++17
  TextView tv(act);
  tv.setText("Hello from Native C++");
  tv.setTextSize(24.0f);
  btn.setOnClickListener([]() { toast("Clicked!"); });
  ```

* **⚡ Native Processing Pipelines:**  
  Hardware access, sensor listeners, compute workloads, and UI event orchestration stay inside the native runtime. Intermediate buffers can flow between native libraries via direct pointers without round-tripping through custom Java bridge classes. Direct memory buffers and `ANativeWindow` handles are supported natively.

* **🔌 Universal, C-Friendly ABI (`stratum_c_abi.h`):**  
  The engine core compiles into a standalone, pure C ABI library (`libstratum.so`). Because the interface is exported via standard C calling conventions, **other programming languages (C, C++, Rust, Zig, Go, or custom runtimes)** can directly call Android APIs without writing custom JNI wrappers.

---

## Technical Architecture

Stratum uses a fixed-slot dispatch mechanism instead of runtime reflection:

```
[ Your Native Code (C++17 ) ]
                  │
                  ▼
   [ Header-Only Typed C++ Wrappers ]
                  │
                  ▼
       [ stratum_c_abi.h ]
   (stratum_call_v, _z, _i, _j, _d, _str, _o, _arr)
                  │
                  ▼
         [ libstratum.so ]
   (Lazy Class Resolution + Cached jmethodID Table)
                  │
                  ▼
         [ Android JVM / ART ]
```

1. **Deterministic Indexing:** The offline pipeline indexes every class (`class_id`) and method (`slot`). Overloads and method pointers are assigned integer slots at generation time.
2. **String Pool & Metadata Table:** `libstratum.so` embeds an offset-based string table and class metadata array (`g_classes`). Method names and JNI signatures are never parsed at runtime.
3. **Lazy Resolution:** Classes and `jmethodID` arrays are resolved and cached on first call per class using a thread-safe mutex and double-checked atomic flags.
4. **Tag-Driven Parameter Marshalling:** Each method carries a compact `param_tags` string (`Z`, `B`, `s`, `[`, `A`, `a`, `p`, etc.). The dispatcher reads the tag and packs arguments directly into JNI `jvalue` arrays with no signature parsing.
5. **Abstract Class & Interface Adapters:** Dynamic proxies (`java.lang.reflect.Proxy`) cannot subclass abstract classes. Stratum detects abstract callback patterns (`CameraDevice.StateCallback`, `WebViewClient`, `TextWatcher`) and generates concrete Java adapter source files (`Adapter_<fqn>.java`) that route calls back to native handlers via `StratumInvocationHandler.nativeDispatch`.

---

## Requirements

- **Host System:** Linux, macOS, or Windows
- **Python:** 3.10+
- **JDK:** Version 17 or 21 (needs `javap` accessible via `PATH` or configured path)
- **Android Target:** Android SDK platform archive (`android.jar` for the target API level)
- **Target Project:** Android Studio with NDK and CMake 3.22.1+

---

## Automated Pipeline Setup

The automated pipeline runner (`run_pipeline_helper.py`) downloads the required platform tools and JDKs, extracts bytecode, parses signatures, runs sanitization, and packages the complete SDK.

### 1. Configure Build Targets (`pipeline.json`)

Configure the target API level and file outputs in `pipeline.json`:
```json
{
  "jobs": [
    {
      "name": "android-35",
      "enabled": true,
      "api_version": "35",
      "android_jar_url": "https://dl.google.com/android/repository/platform-35_r01.zip",
      "zip_dir": "{api_dir}/21_cpp_emit/release",
      "zip_name": "stratum_cpp_android_api_{api}.zip",
      "api_versions_xml": "auto",
      "hiddenapi_flags": "skip",
      "options": {
        "sanitize_strict": true,
        "abstract_mode": "on",
        "runtime_min_sdk": 24,
        "runtime_java_version": "11"
      }
    }
  ]
}
```

### 2. Run the Pipeline

Inspect configuration and stages:
```bash
python run_pipeline_helper.py --list
```

Dry-run to verify commands and target directories without execution:
```bash
python run_pipeline_helper.py --dry-run
```

Execute the full build pipeline for an API:
```bash
python run_pipeline_helper.py --job 35
```

Partial execution options:
```bash
# Run only specific stages (by prefix)
python run_pipeline_helper.py --only 20,21

# Resume pipeline starting from a specific stage
python run_pipeline_helper.py --from 04_5_sanitize
```

### 3. Generated SDK Layout

When the pipeline finishes, the packaged SDK is located at:
```
build/API_<version>/21_cpp_emit/output/stratum/
├── README.md
├── cpp/
│   ├── CMakeLists.txt
│   ├── native-lib.cpp
│   └── stratum/              # libstratum.so sources, CMakeLists.txt, include/
└── stratum-runtime/          # Android Library module (.aar format)
    ├── build.gradle
    ├── consumer-rules.pro
    └── src/main/java/com/stratum/{runtime,adapters}/
```

---

## Integrating into an Android Studio Project

### 1. Copy Files into the App Project
From `build/API_<version>/21_cpp_emit/output/stratum/`:
1. Copy the `stratum-runtime` folder into your Android project root (next to `app/`).
2. Copy the `cpp/stratum` directory into your native code directory: `app/src/main/cpp/stratum/`.

### 2. Update Gradle Configuration
In your project-level `settings.gradle`:
```groovy
include ':app'
include ':stratum-runtime'
```

In your application module `app/build.gradle`:
```groovy
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags "-std=c++17"
            }
        }
    }
}

dependencies {
    implementation project(':stratum-runtime')
}
```

### 3. Configure CMake
In `app/src/main/cpp/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.22.1)
project(myapp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiles libstratum.so and exports PUBLIC headers
add_subdirectory(stratum)

add_library(myapp SHARED native-lib.cpp)

find_library(log-lib log)
target_link_libraries(myapp PRIVATE stratum ${log-lib})
```

### 4. Wire the Activity
In `MainActivity.java`:
```java
package com.example.myapp;

import com.stratum.runtime.StratumActivity;

public class MainActivity extends StratumActivity {
    @Override
    protected boolean onImportMain() {
        // Loads your application native library after libstratum is loaded
        System.loadLibrary("myapp");
        return true;
    }
}
```

---

## C++ SDK Guide

### 1. Lifecycle Entry Point
Define `onCreate` in the global namespace in your native library. Stratum auto-wires lifecycle calls via weak symbols:

```cpp
#include <stratum/stratum.hpp>
#include <stratum/android/widget/LinearLayout.hpp>
#include <stratum/android/widget/TextView.hpp>
#include <stratum/android/widget/Button.hpp>
#include <stratum/android/widget/Toast.hpp>

using stratum::Activity;
using stratum::JStr;
using stratum::Object;
using stratum::android::widget::LinearLayout;
using stratum::android::widget::TextView;
using stratum::android::widget::Button;
using stratum::android::widget::Toast;

static inline int32_t C(uint32_t c) { return static_cast<int32_t>(c); }

void onCreate(Activity act) {
    LinearLayout root(act);
    root.setOrientation(LinearLayout::VERTICAL);
    root.setPadding(32, 48, 32, 32);

    TextView label(act);
    label.setText(JStr("Hello from Stratum Native C++"));
    label.setTextSize(22.0f);
    label.setTextColor(C(0xFFFFFFFF));
    root.addView(label);

    Button btn(act);
    btn.setText(JStr("Show Toast"));
    btn.setOnClickListener([act]() {
        Toast::makeText(act, JStr("Clicked from C++"), Toast::LENGTH_SHORT)
             .unchecked_as<Toast>()
             .show();
    });
    root.addView(btn);

    stratum::setContentView(act, root);
}
```

### 2. Core Operational Rules

1. **Default Constructing an Object produces a `NULL` reference:**  
   `Paint p;` is null. Calling methods on it throws `JavaException`. To construct a no-arg Java object, call its static `create()` method:
   ```cpp
   Paint p = Paint::create(); // calls new Paint()
   ```
2. **Object Returns are Untyped (`stratum::Object`):**  
   Cast to specific wrappers using `.as<T>()` (checked via JNI `IsInstanceOf`) or `.unchecked_as<T>()`:
   ```cpp
   SensorManager sm = act.getSystemService("sensor").as<SensorManager>();
   ```
3. **Strings:**  
   - Inputs accept `stratum::JStr` (implicitly constructed from `"literals"`, `std::string`, `std::string_view`, or `nullptr`).
   - String/CharSequence returns are returned as `std::string`.
4. **Single vs. Multi-Method Callbacks:**  
   - Single-method listeners accept a plain lambda:
     ```cpp
     btn.setOnClickListener([]() { ... });
     ```
   - Multi-method listeners or abstract classes use `stratum::Handlers`:
     ```cpp
     seek.setOnSeekBarChangeListener(stratum::Handlers()
         .on("onProgressChanged", [](Object s, int32_t p, bool user) { ... })
         .on("onStartTrackingTouch", [](Object s) {})
         .on("onStopTrackingTouch",  [](Object s) {}));
     ```
5. **Background Threads:**  
   Calls that mutate UI views must be posted to the main thread:
   ```cpp
   stratum::run_on_ui_thread([label, text]() {
       label.setText(JStr(text));
   });
   ```
6. **Exception Safety in Callbacks:**  
   Exceptions escaping C++ callbacks become Java runtime exceptions. Wrap internal callback logic in `try / catch`:
   ```cpp
   btn.setOnClickListener([]() {
       try {
           // operations
       } catch (const std::exception& e) {
           __android_log_print(ANDROID_LOG_ERROR, "App", "%s", e.what());
       }
   });
   ```

---

## Dynamic Reflection (`stratum/reflect.hpp`)

Stratum's generated headers cover platform classes found in `android.jar`. For classes outside `android.jar` (e.g., `androidx.*`, third-party libraries, or your own app-specific Java classes), use `stratum::reflect`:

```cpp
#include <stratum/reflect.hpp>

// Static method invocation
int32_t status = stratum::reflect::call_static<int32_t>(
    "androidx.core.content.ContextCompat",
    "checkSelfPermission",
    act,
    std::string("android.permission.CAMERA")
);

// Instantiate arbitrary Java classes
Object myHelper = stratum::reflect::new_instance("com.example.MyHelper", act, 42);

// Instance method invocation
stratum::reflect::call_method<void>(myHelper, "doWork", std::string("payload"));
```

---

## Universal C ABI Reference (`stratum_c_abi.h`)

All C++ wrappers evaluate down to the C ABI exported by `libstratum.so`. Any language with C-FFI support can bind directly against this header:

```c
// Method Invocation
int64_t stratum_new_instance(uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
void    stratum_call_v(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
int64_t stratum_call_i(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
double  stratum_call_d(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
int64_t stratum_call_o(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);

// Reference Lifecycle (global ref handles)
void    stratum_delete_ref(int64_t p);
int64_t stratum_clone_ref(int64_t p);

// Callbacks & Lifecycle
void    stratum_register_callback(const char* key, const StratumCbDesc* d);
void    stratum_remove_callback(const char* key);
int32_t stratum_post_ui(const StratumCbDesc* d);
int64_t stratum_get_activity(void);

// Surface & Native Buffers
void*   stratum_surface_to_native_window(int64_t surface);
void*   stratum_get_direct_buffer_address(int64_t byte_buffer);
size_t  stratum_get_direct_buffer_capacity(int64_t byte_buffer);
```

Every function accepting `int32_t* err` sets `*err = 0` on success and `*err = 1` on failure. The detailed error message can be inspected via `stratum_last_error()`.

---

## Related Projects

- **Stratum (Python / nanobind):**  
  For running the full pipeline generating CPython / nanobind native extensions for Android:  
  [https://github.com/a-s-l-a-h/stratum](https://github.com/a-s-l-a-h/stratum)

- **Stratum Android Python 3.14 Demo:**  
  Working Android Studio project template demonstrating embedded Python runtime integration:  
  [https://github.com/a-s-l-a-h/stratum_android_py_3_14](https://github.com/a-s-l-a-h/stratum_android_py_3_14)

---




---
---
---
## Automated 

1) First clone this  repo 
2) run this : python run_pipeline_helper.py               # full run for all enabled jobs
3) wait take some time to compleate 
4) aftet compleation look inside build/API_35/21_cpp_emit/output/  inside it stratum folder you can copy that and inside that a readme.md there you can refer to how to use this.

---

## Coustomize details

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
python 21_cpp_emit/package.py --input "build/API_35/21_cpp_emit/output" --generated "build/API_35/20_engine_core/generated" --adapters "build/API_35/05_5_abstract/output/java/com/stratum/adapters" --output "build/API_35/21_cpp_emit/output/stratum" --api-version 35 --min-sdk 24 --java-version 11
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

