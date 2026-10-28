# Third-Party Licenses & Notices

stratum-sys itself is released under the MIT License (see `LICENSE`). It depends on the tools and data below. This document is a notice of those dependencies and links to their terms. It is not legal advice.

## Table of Contents

1. [Tools used when running the pipeline (not shipped)](#1-tools-used-when-running-the-pipeline-not-shipped)
2. [Data the pipeline reads](#2-data-the-pipeline-reads)
3. [Code that ends up in an app built with Stratum](#3-code-that-ends-up-in-an-app-built-with-stratum)

---

## 1. Tools used when running the pipeline (not shipped)

These are used while generating code. They are not part of the generated output.

### Python
- **Version:** 3.10 or newer
- **License:** Python Software Foundation License
- **Usage:** runs `run_pipeline_helper.py` and every stage script. The pipeline uses only the Python standard library; no third-party Python packages are required.
- **Source:** https://docs.python.org/3/license.html

### Java Development Kit (Eclipse Temurin / OpenJDK)
- **Version:** JDK 17, downloaded into `build/API_<n>/` by the pipeline (URL set in `pipeline.json`). Any OpenJDK build works.
- **License:** GNU General Public License v2 with the Classpath Exception
- **Usage:** `javap` (stage 03) reads `android.jar` classes. No JDK code is copied into the output.
- **Source:** https://adoptium.net/ and https://openjdk.org/
- **License details:** https://openjdk.org/legal/gplv2+ce.html

### Android SDK, NDK and CMake
- **License:** Android Software Development Kit License Agreement
- **Provider:** Google LLC
- **Usage:** building `libstratum.so` and the Android app. Installed through Android Studio.
- **Terms:** https://developer.android.com/studio/terms

## 2. Data the pipeline reads

### android.jar (platform API)
- **Source:** the Android SDK platform package for the chosen API level (API 35 by default); URL set in `pipeline.json`.
- **Usage:** parsed to produce the metadata table, C++ headers and adapters. The jar is downloaded by the user's machine and is not redistributed by this project.
- **Terms:** Android SDK License Agreement (above). The Android platform source is licensed under Apache License 2.0 unless noted: https://source.android.com/docs/setup/about/licenses

### api-versions.xml
- **Source:** extracted from the platform zip, or supplied from `third_party/api_versions/<api>/`.
- **Usage:** records which API level each class and method was added in.
- **License:** part of the Android SDK; see the terms above.

### hiddenapi-flags.csv
- **Source:** downloaded from the URL configured for the pipeline, or supplied from `third_party/hiddenapi/<api>/`.
- **Usage:** used to leave hidden and blocked APIs out of the generated output.
- **License:** derived from the Android Open Source Project (Apache License 2.0). *Maintainer: confirm the exact source URL and add it here.*

The generated output contains the names and signatures of public Android APIs. Check the Android SDK terms before redistributing generated files.

## 3. Code that ends up in an app built with Stratum

### Android NDK runtime (libc++)
- **License:** Apache License 2.0 with LLVM Exceptions
- **Usage:** statically linked into `libstratum.so` (`-DANDROID_STL=c++_static`).
- **Source:** https://libcxx.llvm.org/ and https://developer.android.com/ndk

### stratum-runtime (Java module)
- **License:** MIT (this project). It runs on top of the Android framework and AndroidX AppCompat, which the app already includes (Apache License 2.0): https://developer.android.com/jetpack/androidx