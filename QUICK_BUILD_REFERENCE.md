

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


























####


# Run commands old for quick look
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




# 1. engine
python 20_engine_core/emit.py  --input 05_resolve/output_patched
python 20_engine_core/build.py --abi arm64-v8a            # add --log for deep tracing; repeat for x86_64
# -> 20_engine_core/output/arm64-v8a/libstratum.so

# 2. SDK (start restricted; drop --emit-prefix later for the whole SDK)
python 21_cpp_emit/main.py --input 05_resolve/output_patched

python 21_cpp_emit/package.py  # when this run in output folder in stratum there is folder create stratum inside it all c and cpp code for stratum and cmake also there so you can copy and place in the android studio project and build there .

---


