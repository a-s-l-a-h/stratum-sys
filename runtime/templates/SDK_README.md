# Stratum C++ SDK

Generated for **Android API @@COMPILE_SDK@@** (meta hash `@@META_HASH@@`).
Write Android apps in C++ with typed wrappers for the Android SDK.

---

## 0. What is in this package

```
stratum/                              <- the folder you unzipped
├── README.md                         <- this file
├── cpp/                              <- goes into  app/src/main/cpp/
│   ├── CMakeLists.txt                   app-side CMake (builds YOUR library)
│   ├── native-lib.cpp                   small demo, replace with your code
│   └── stratum/                         the engine (builds libstratum.so)
│       ├── CMakeLists.txt
│       ├── src/                         engine sources
│       ├── generated/                   metadata table (do not edit)
│       └── include/                     C++ headers  (<stratum/...>)
└── stratum-runtime/                  <- goes into  <project root>/stratum-runtime
    ├── build.gradle                     Android library module
    ├── consumer-rules.pro               keep rules (applied automatically)
    └── src/main/
        ├── AndroidManifest.xml          Service + Receiver (merged automatically)
        └── java/com/stratum/
            ├── runtime/                 StratumActivity and friends
            └── adapters/                generated callback adapters
```

## 1. Where everything ends up in your project

Before:
```
MyProject/
├── settings.gradle(.kts)
├── build.gradle(.kts)
└── app/
    ├── build.gradle(.kts)
    └── src/main/
        ├── AndroidManifest.xml
        └── java/com/example/myapp/MainActivity.java
```

After you copy the two folders:
```
MyProject/
├── settings.gradle(.kts)             <- EDIT  (step 2)
├── build.gradle(.kts)
├── stratum-runtime/                  <- COPIED from stratum/stratum-runtime/
└── app/
    ├── build.gradle(.kts)            <- EDIT  (step 4)
    └── src/main/
        ├── AndroidManifest.xml       <- EDIT  (step 6)
        ├── cpp/                      <- COPIED from stratum/cpp/ (merge contents)
        │   ├── CMakeLists.txt
        │   ├── native-lib.cpp
        │   └── stratum/
        └── java/com/example/myapp/MainActivity.java   <- EDIT (step 5)
```

## 2. Add the runtime module

Copy `stratum/stratum-runtime/` to `MyProject/stratum-runtime/`, next to `app/`.
Then in `settings.gradle` (Groovy) or `settings.gradle.kts` (Kotlin):

    include(":app", ":stratum-runtime")

(If your file already has `include(":app")`, just add `":stratum-runtime"` to it.)

## 3. Add the native code

Copy everything inside `stratum/cpp/` into `MyProject/app/src/main/cpp/`
(create the `cpp` folder if it does not exist). You get:

- `CMakeLists.txt`  - builds your library `myapp` and links the engine
- `native-lib.cpp`  - demo; replace it with your own code
- `stratum/`        - the engine

Already have a `CMakeLists.txt` in that folder? Keep yours and add only:

    add_subdirectory(stratum)
    target_link_libraries(<your-library> PRIVATE stratum ${log-lib})

## 4. Edit `app/build.gradle(.kts)`

Kotlin DSL (`build.gradle.kts`):

    android {
        compileSdk = @@COMPILE_SDK@@            // must be >= @@COMPILE_SDK@@

        defaultConfig {
            minSdk = @@MIN_SDK@@                // must be >= @@MIN_SDK@@
            ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
            externalNativeBuild { cmake { cppFlags += "-std=c++17" } }
        }
        externalNativeBuild {
            cmake { path = file("src/main/cpp/CMakeLists.txt") }
        }
    }

    dependencies {
        implementation(project(":stratum-runtime"))
    }

Groovy DSL (`build.gradle`):

    android {
        compileSdk @@COMPILE_SDK@@

        defaultConfig {
            minSdk @@MIN_SDK@@
            ndk { abiFilters "arm64-v8a", "x86_64" }
            externalNativeBuild { cmake { cppFlags "-std=c++17" } }
        }
        externalNativeBuild {
            cmake { path file("src/main/cpp/CMakeLists.txt") }
        }
    }

    dependencies {
        implementation project(':stratum-runtime')
    }

Your app's Java level (`compileOptions`) must be equal to or higher than the one
in `stratum-runtime/build.gradle`.

## 5. Your Activity

Extend `StratumActivity` and load YOUR native library in `onImportMain()`:

    package com.example.myapp;

    import com.stratum.runtime.StratumActivity;

    public class MainActivity extends StratumActivity {
        @Override
        protected boolean onImportMain() {
            System.loadLibrary("myapp");   // same name as add_library(myapp ...) in CMakeLists.txt
            return true;
        }
    }

`libstratum.so` is loaded automatically by `StratumActivity`.

## 6. Manifest

Declare your Activity as usual, with an **AppCompat-based theme**:

    <application
        android:theme="@style/Theme.MyApp">
        <activity
            android:name=".MainActivity"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>

The runtime module adds `StratumService`, `StratumReceiver` and the
`RECEIVE_BOOT_COMPLETED` permission automatically. If you do not need them,
remove them in your own manifest:

    <service android:name="com.stratum.runtime.StratumService" tools:node="remove" />
    <receiver android:name="com.stratum.runtime.StratumReceiver" tools:node="remove" />

(add `xmlns:tools="http://schemas.android.com/tools"` to the `<manifest>` tag).

## 7. Write C++ and run

`native-lib.cpp` contains a working demo:

    #include <stratum/stratum.hpp>
    void onCreate(stratum::Activity act) { /* build your UI here */ }

Header paths mirror the Android package names, e.g. `android.widget.Button` is
`<stratum/android/widget/Button.hpp>` in namespace `stratum::android::widget`.
Browse `cpp/stratum/include/stratum/API_INDEX.txt` for every available call.

Press Run in Android Studio.

## Requirements checklist

- [ ] `stratum-runtime/` copied to the project root and included in `settings.gradle`
- [ ] `cpp/` contents copied into `app/src/main/cpp/`
- [ ] `app` `compileSdk` >= @@COMPILE_SDK@@ and `minSdk` >= @@MIN_SDK@@
- [ ] `implementation(project(":stratum-runtime"))` in `app/build.gradle`
- [ ] NDK + CMake installed (SDK Manager -> SDK Tools)
- [ ] Activity extends `StratumActivity`, library name matches `System.loadLibrary(...)`
- [ ] AppCompat theme on the application or activity

## Troubleshooting

| Symptom | Cause |
|---|---|
| `UnsatisfiedLinkError: dlopen failed: library "libmyapp.so" not found` | Name in `System.loadLibrary` does not match `add_library(...)` |
| `libstratum.so ... and SDK headers ... generated from different inputs` | `cpp/stratum/generated` and `include` come from different pipeline runs: re-copy the whole `cpp/` folder |
| `class not found on this device` | Device API is older than the SDK API, or the class/method is not available on that Android version |
| `You need to use a Theme.AppCompat theme` | Set an AppCompat/Material theme on the Activity |
| `Duplicate class com.stratum...` | You still have the old `src/main/cpp/stratum/java` source dir in `app/build.gradle`: remove it |
| Header not found (`<stratum/android/...>`) | That class was not emitted: check `emit_prefix` in `pipeline.json` and re-run the pipeline |

Compile-time tracing: `-DSTRATUM_LOG_ENABLED=1` (toggle with `stratum::set_log_enabled`).