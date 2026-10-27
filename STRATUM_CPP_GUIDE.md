# STRATUM C++ SDK — COMPLETE DEVELOPER GUIDE & LLM SKILL

> **HOW TO USE THIS FILE**
> Paste it whole into an LLM (system prompt / project knowledge). The LLM then needs no pipeline source.
> Everything below was checked against the real pipeline + engine + header-emitter source.

---

## 0. ROLE DIRECTIVE (for the LLM)

You are a **Stratum C++ library expert**. You write Android apps in modern C++17 using the Stratum SDK
(`#include <stratum/...>`). Rules you MUST follow:

1. Never write raw JNI (`JNIEnv*`, `FindClass`, `GetMethodID`, `jobject`) in app code. Use the generated typed headers,
   or `stratum::reflect` for classes that are not in `android.jar`.
2. Never invent APIs. Only use what this guide documents or what the user shows in the generated headers / `API_INDEX.txt`.
   If unsure whether a method exists or what its C++ signature is, **ask the user to grep `stratum/include/stratum/API_INDEX.txt`**.
3. Follow the "10 Golden Rules" (section 3) and the "Gotchas" (section 11).
4. Output complete, compilable files (includes + `using` + code). Always say which Java/Gradle/CMake/Manifest changes are also needed.

---

## 1. WHAT STRATUM IS & HOW EACH LAYER IS BUILT

Stratum turns the **whole Android SDK (`android.jar`)** into (a) a metadata table compiled into `libstratum.so` and
(b) header-only C++ wrapper classes. App code calls typed C++ methods; each call becomes
`stratum_call_*(object, class_id, slot, args)`; the engine looks up a **cached** `jmethodID` by `(class_id, slot)` and calls
`Call<Type>MethodA`. No JNI signature strings are parsed at runtime.

### 1.1 Offline pipeline (Python, run once per Android API level)

| Stage | What it does |
|---|---|
| `00_setup` | Validates Python 3.10+, `javap` (JDK), `android.jar` (+ optional `api-versions.xml`, `hiddenapi-flags.csv`). Writes `setup_report.json`. |
| `01_extract` | Unzips every `.class` from `android.jar`. |
| `02_inspect` | Lists all FQNs; `targets.json` (`mode: full` = every class). |
| `03_javap` | Runs `javap -s -p -constants` per class → exact JNI descriptors + constant values. |
| `04_parse` | javap text → per-class JSON (methods, fields, params, return types, parent, interfaces). Maps each Java type to a conversion kind. Flags params whose type name ends in `Listener/Callback/Observer/Runnable` as "needs proxy". |
| `04_5_sanitize` | Safety filter: drops `private` members and hidden-API `blocked` members (they crash ART under CheckJNI). `--strict` also drops restricted/unsupported/conditional hidden APIs. |
| `05_resolve` pass 1 | Assigns deterministic `class_id` (0..N-1, sorted by FQN) and per-class method `slot` (constructors first, then sorted by name+signature). Computes `param_tags` (one char per parameter) and `ret_type_id`. |
| `05_5_abstract` | Generates **real Java adapter classes** `com.stratum.adapters.Adapter_<fqn>` for abstract callback classes (`WebViewClient`, `CameraDevice$StateCallback`, …) and for seeded multi-method interfaces (`TextWatcher`, `SensorEventListener`, …). `java.lang.reflect.Proxy` can only implement interfaces, so abstract classes need these. Every callback method forwards to `StratumInvocationHandler.nativeDispatch(key, "methodName", Object[] args)`. |
| `05_resolve` pass 2 | Re-runs on the patched JSON: params that now have an adapter get tag `a`. |
| `20_engine_core` | Emits `metadata_table.cpp/.h` (deduplicated string pool + `ClassMeta`/`MethodMeta`/`FieldMeta` arrays) and `meta_hash.h`. Builds `libstratum.so`. |
| `21_cpp_emit` | Emits one header per class under `<stratum/android/...>`. Inherited methods are **flattened** into each class (called with the *declaring* class's id). Emits `core.hpp`, `stratum.hpp`, `reflect.hpp`, `meta_hash.hpp`, `API_INDEX.txt`. |
| `21b package` | Assembles the `stratum/` folder (CMake, engine src, generated table, headers, Java runtime + adapters, manifest snippet, proguard rules) → zip. |

`STRATUM_META_HASH` in the headers must equal the one in `libstratum.so` (checked at load, aborts if different) — headers and engine must come from the same pipeline run.

### 1.2 Parameter tags (precomputed, drive the engine's argument packing)

| Tag | Java param | C++ param type in generated header |
|---|---|---|
| `Z B C S I J F D` | boolean byte char short int long float double | `bool int8_t uint16_t int16_t int32_t int64_t float double` (`Z` becomes `stratum::Bool` in some overload sets, see Gotchas) |
| `s` | `String`/`CharSequence` | `stratum::JStr` |
| `[ ] q f d b c h` | `byte[] int[] long[] float[] double[] boolean[] char[] short[]` | `stratum::Arr<uint8_t/int32_t/int64_t/float/double/uint8_t/uint16_t/int16_t>` |
| `T` | `String[]` | `const stratum::StrArr&` |
| `A` | any other object array | `const stratum::ObjArr&` |
| `M` | `List/Collection/ArrayList/Iterable` | `const stratum::ListArg&` |
| `N` | `Map/HashMap/…` | `const stratum::MapArg&` |
| `a` | abstract class / interface that has a generated adapter | template `C&&` (lambda, `Handlers`, or an existing listener `Object`) |
| `p` | single-method listener, dynamic proxy | template `C&&` (lambda or an existing `Object`) |
| `L` | everything else (incl. boxed `Integer` etc. and **un-adapted functional interfaces**) | `const stratum::Object&` (boxed types: the primitive, boxed for you) |

### 1.3 Runtime layers

```
Your C++ app  ->  <stratum/...> header-only wrappers (RAII Object, typed classes)
              ->  C ABI  (stratum_c_abi.h:  stratum_call_v/z/i/j/d/str/o/arr/list, fields, refs, callbacks)
              ->  libstratum.so  (lazy class resolve + jmethodID cache, thread attach/detach, proxy/adapter factory)
              ->  ART
Java side: com.stratum.runtime.{StratumActivity, StratumInvocationHandler, StratumView, StratumService, StratumReceiver, StratumRuntimeLookup, StratumBootstrap}
           com.stratum.adapters.Adapter_*   (generated)
```

---

## 2. PROJECT SETUP

```
app/src/main/cpp/
  CMakeLists.txt
  main.cpp
  stratum/            <- unzipped SDK (CMakeLists.txt, src/, generated/, include/, java/, android/, consumer-rules.pro)
```

**`app/src/main/cpp/CMakeLists.txt`**
```cmake
cmake_minimum_required(VERSION 3.22.1)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(stratum)                       # builds libstratum.so (exports <stratum/...> headers PUBLIC)
add_library(myapp SHARED main.cpp)
find_library(log-lib log)
target_link_libraries(myapp PRIVATE stratum ${log-lib})
```
(Option: `-DSTRATUM_LOG_ENABLED=1` compiles in tracing; toggle at runtime with `stratum::set_log_enabled(true)`; logcat tags `Stratum`, `StratumTrace`.)

**`app/build.gradle(.kts)`** – ABI filters (`arm64-v8a`, `x86_64`), `externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }`, dependency `androidx.appcompat:appcompat`
(StratumActivity extends AppCompatActivity). Get the Java runtime + adapters into the build **in exactly one way**:
- `java.srcDir("src/main/cpp/stratum/java")`, **or**
- a library module (e.g. `:stratum-runtime`) containing them.
Not both (duplicate classes). Whichever way, `compileSdk` must be >= the API level the SDK was generated from (adapters reference those classes).

**`MainActivity.java`**
```java
public class MainActivity extends com.stratum.runtime.StratumActivity {
    @Override protected boolean onImportMain() {
        System.loadLibrary("myapp");   // YOUR lib. libstratum is loaded by StratumActivity first.
        return true;
    }
}
```
Order: `StratumActivity.onCreate` → loads `libstratum.so` → `nativeSetActivity` → `onImportMain()` (your lib loads, its static init wires lifecycle) → `nativeOnCreate()` → your C++ `onCreate(Activity)`.

**Manifest**: declare your `MainActivity`, `<uses-permission>`s, `<uses-feature>`s as in normal Android. Merge `stratum/android/AndroidManifest.snippet.xml` only if you use `StratumService` / `StratumReceiver`.
**R8/ProGuard**: keep `com.stratum.runtime.**` and `com.stratum.adapters.**` (shipped in `consumer-rules.pro`) — they're found only by JNI string lookup.

---

## 3. THE 13 GOLDEN RULES

1. **`Cls()` is a NULL object.** A Java `new Foo()` with no args is **`Foo::create()`**. Constructors with args are normal: `Foo f(a, b);`. Calling a method on a null object throws `JavaException`.
2. **Object-returning methods return untyped `stratum::Object`.** Downcast with `.as<T>()` (checked, JNI `IsInstanceOf`, throws if wrong) or `.unchecked_as<T>()` (free).
3. **Strings in = `stratum::JStr`** (implicit from `const char*`, `std::string`, `std::string_view`, `nullptr`). **Strings out = `std::string`.** Hold a persistent Java String with `stratum::java_string("x")`.
4. **Namespaces mirror packages; nested classes use `_` instead of `$`**: `android.graphics.Paint$Style` → `stratum::android::graphics::Paint_Style`, header `<stratum/android/graphics/Paint_Style.hpp>`. Always include the header of every class you use (they are self-contained; parents are not needed).
5. **Static final primitive/String constants are `static constexpr`** (`View::VISIBLE`). Other static fields: `Cls::sf_get_NAME()` / `sf_set_NAME(v)`. Instance fields: `obj.f_get_name()` / `f_set_name(v)`.
6. **Single-method listeners (tag `p`) take a plain lambda. Multi-method listeners / abstract callback classes (tag `a`) take `stratum::Handlers().on("method", lambda)`.**
7. **Wrap every non-trivial callback body in try/catch (`guard`)**. A C++ exception escaping a callback becomes a Java `RuntimeException` on the calling thread (usually the UI thread) → app crash.
8. **Color/int literals: cast with `C()`** (`static_cast<int32_t>`). `0xFF00BFA5` is `unsigned` and makes overloads like `setColor(int)` vs `setColor(long)` ambiguous.
9. **UI mutations from other threads → `stratum::run_on_ui_thread([=]{ ... })`.** Lifecycle hooks and view listeners already run on the UI thread.
10. **Never store raw handles (`int64_t`/`jobject`).** Store `stratum::Object` or a typed wrapper. Each owns one JNI global ref (RAII: copy = clone ref, move = steal, destructor = delete ref).
11. **Do NOT mark lambdas `mutable` unless you really assign to a by-value capture.** Every Stratum `Object`/wrapper method is `const`, so `label.setText(..)` works inside a normal lambda. See section 3.1.
12. **Prefer the typed API; use `stratum::reflect` only when no header exists** (androidx, third-party, your own Java classes). Typed calls use a cached slot dispatch; reflection resolves by name and scores overloads on every call. See section 9.
13. **Source is UTF-8: never use a non-ASCII character as a C++ `char` literal** (`'×'`, `'÷'`, `'√'` fail: "character too large for enclosing character literal type"). Keep internal codes ASCII (`'*'`, `'/'`) and use the pretty glyph only as a `std::string` label (`"×"`). See section 3.2.

### 3.1 C++ LAMBDA RULES (the #1 source of compile errors in generated code)

**Why `mutable` breaks things.** A `mutable` lambda has a **non-const** `operator()`. A captured copy inside another (non-`mutable`) lambda is **const**, so calling it there fails:

```
error: no matching function for call to object of type 'const (lambda at native-lib.cpp:75)'
note: 'this' argument has type 'const (lambda ...)', but method is not marked const
```

```cpp
// BAD: onButtonClick is mutable (non-const call operator) ...
auto onButtonClick = [display](const std::string& key) mutable { display.setText(key); };
btn.setOnClickListener([onButtonClick, text]() {   // ... captured copy is const here
    onButtonClick(text);                            // ERROR
});

// GOOD 1 (preferred): drop `mutable`. Stratum Object methods are const, nothing is modified.
auto onButtonClick = [display](const std::string& key) { display.setText(key); };
btn.setOnClickListener([onButtonClick, text]() { onButtonClick(text); });

// GOOD 2: wrap in std::function (copyable, const-callable)
std::function<void(const std::string&)> onButtonClick = [display](const std::string& key) { display.setText(key); };

// GOOD 3: you really mutate a capture (e.g. a counter) -> make the enclosing lambda mutable too,
//         or keep the state in a `static`/heap variable instead of a by-value capture.
```

Rules:
- Default to `[=]`/`[a, b]` **without** `mutable`. Only write `mutable` if you assign/increment a by-value capture, and then every lambda that calls it must also be `mutable`.
- Mutable state shared between callbacks: use `static int counter;` or `std::shared_ptr<State>`, not a by-value capture.
- Helper lambdas used from several callbacks: declare them as non-capturing `static` functions, or `std::function`, or pass the captures explicitly.
- Capture Stratum objects **by value** (`[label]`): the copy owns its own JNI global ref and outlives the scope. Never capture locals by reference (`[&]`) in a callback that runs later; the stack frame is gone.
- A lambda handed to Stratum is stored (type-erased); it must be copy- or move-constructible. Do not capture `std::unique_ptr` by copy (use `std::shared_ptr`).
- Parameter types of the lambda must match the Java callback: `int32_t`, `bool`, `float`, `double`, `std::string`, `stratum::Object`. Arity must equal the Java method arity (0-8).
- `[=]` captures `this` implicitly only inside member functions: prefer explicit captures.

### 3.2 TEXT / CHARACTER RULES

- `char` holds ONE byte. `'×' '÷' '−' '√' 'π' '€'` are multi-byte UTF-8 -> **compile error** (`character too large for enclosing character literal type`).
- Non-ASCII goes in **string literals only**: `"×"`, `"÷"` (type `const char*`/`std::string`, UTF-8, passes to `JStr` unchanged).
- Pattern: separate the **logic code** (ASCII, `char` or `std::string`) from the **display label** (any UTF-8 string).

```cpp
struct Key { const char* label; char op; };       // label may be UTF-8, op is ASCII
static const Key kOps[] = { {"÷", '/'}, {"×", '*'}, {"-", '-'}, {"+", '+'} };
// compare 'op' (char) in the logic; show 'label' on the Button.
```
- Do not compare strings with `<`/`>` to test for digits (`key >= "0" && key <= "9"` is lexicographic and fragile). Use `key.size() == 1 && std::isdigit(static_cast<unsigned char>(key[0]))` (`<cctype>`).
- `std::stod` throws on bad input; it is fine inside `guard`, but pre-validate or keep the parsed number in your state struct instead of re-parsing display text.
- Prefer keeping **state in your own struct** (`std::shared_ptr<State>` or a `static`), not reading it back from the TextView (`getText()` is a JNI round-trip).

---

## 4. NAMING / GENERATION RULES (exact)

| Java | Stratum C++ |
|---|---|
| `new Foo(ctx)` | `Foo f(ctx);` (explicit ctor, only if it has params and no callback params) |
| `new Foo()` | `Foo f = Foo::create();` |
| ctor with a listener param, or duplicate C++ signature | `static Foo create_<ParamTypes>(...)` (e.g. `create_Context_AttributeSet`) |
| abstract class / interface / enum / annotation | **no constructor generated** |
| instance method | `obj.method(args)` (const member) |
| static method | `Cls::method(args)` — **only statics declared on that class itself, not inherited ones** |
| inherited / interface / `Object` methods | flattened into every class; just call them on the subclass |
| C++ keyword or macro name (`delete`, `default`, `new`, `class`, `min`, `max`, `NULL`, …) | trailing `_` → `delete_()`, `min_()` |
| name starting `__` or `_` + uppercase | prefixed `j` |
| collides with `Object` API: `raw release as unchecked_as is_null to_string hash_code is_instance_of adopt create` | trailing `_` → `as_()`, `create_()`; Java `toString()` → use `obj.to_string()`, `hashCode()` → `hash_code()` |
| method name equals class name | trailing `_` |
| two overloads that collapse to the same C++ signature | second gets a type suffix `name_Type1_Type2` (zero-arg: `name_0`) |
| class name equals a sub-package name | class gets trailing `_` |
| `Paint.Style.STROKE` (enum constant object) | `Paint_Style::sf_get_STROKE()` → `Object`, pass it where `Paint.Style` is expected |

**Return types**

| Java return | C++ return |
|---|---|
| `void / boolean / byte / char / short / int / long / float / double` | `void / bool / int8_t / uint16_t / int16_t / int32_t / int64_t / float / double` |
| `String`, `CharSequence` | `std::string` |
| `byte[] int[] long[] float[] double[] boolean[] char[] short[]` | `std::vector<uint8_t/int32_t/int64_t/float/double/bool/uint16_t/int16_t>` |
| `String[]` | `std::vector<std::string>` |
| any other array (`View[]`, `Object[]`) | `std::vector<stratum::Object>` |
| `List/Collection/Set/Queue/…` | `std::vector<stratum::Object>` (snapshot via `toArray()`) |
| `Map/HashMap/…` | `stratum::Object` → `.as_map()` gives `std::vector<std::pair<Object,Object>>` |
| boxed `Integer/Boolean/...` | `stratum::Object` → `.as_i32()`, `.as_bool()`, `.as_i64()`, `.as_float()`, `.as_double()`, `.as_i8()`, `.as_i16()`, `.as_char()` |
| anything else | `stratum::Object` |

**Field setters** exist only for non-final fields of types bool/int/long/float/double/String/Object (float fields take `double`).

---

## 5. CORE TYPES

```cpp
#include <stratum/stratum.hpp>   // includes core.hpp + Activity

stratum::Object o;                       // NULL
o.is_null();  bool(o);                   // null checks
o.raw();  o.release();                   // handle access / give up ownership
stratum::Object::adopt(h);               // take ownership of an existing global ref (rare)
o == p                                   // Java IsSameObject
o.to_string();  o.hash_code();
o.is_instance_of<TextView>();
o.as<TextView>();  o.unchecked_as<TextView>();
o.as_array<int32_t>();                   // also <std::string>, <stratum::Object>, <bool>, <float> ...
o.as_map();
```

**Argument view types** (non-owning; keep the source alive for the duration of the call — a temporary in the call expression is fine):

| Type | Built from |
|---|---|
| `JStr` | `"lit"`, `std::string`, `std::string_view`, `nullptr` |
| `Bool` | only a real `bool` (blocks string-literal→bool conversion). Use `stratum::Bool(true)` when the compiler picks the wrong overload |
| `Arr<T>` | `std::vector<T>`, `{1,2,3}` (exact `T`), `(ptr, n)`, `nullptr` |
| `StrArr` | `std::vector<std::string>`, `{"a","b"}`, `nullptr` |
| `ObjArr` | `std::vector<Object>`, `{o1,o2}`, `nullptr` |
| `ListArg` | `std::vector<Object>`, `{o1,o2}`, an existing Java List `Object`, `nullptr` |
| `MapArg` | `std::vector<std::pair<Object,Object>>`, an existing Java Map `Object`, `nullptr` |

**Creating Java values**
```cpp
stratum::java_string("hi");                                  // java.lang.String
stratum::box_i32(5); box_bool(true); box_i64(1); box_f32(1.f); box_f64(1.0); box_char(u'x'); box_i8; box_i16
stratum::new_array({1,2,3});                                 // int[]
stratum::new_array<uint8_t>({0xDE,0xAD});                    // byte[]
stratum::new_array<int32_t>({{1,2},{3,4}});                  // int[][]   (any depth)
stratum::new_array({"a","b"});  stratum::new_string_array(vec);   // String[]
stratum::new_object_array("android/view/View", vec_of_objects);   // View[]  (elem class "a/b/C", "a.b.C", or "[I" for int[][])
stratum::new_list({o1,o2});   stratum::new_map({{k1,v1},{k2,v2}});
```

**Errors**: any failed Java call throws `stratum::JavaException` (derives `std::runtime_error`; message = Java exception `toString()`). Catch `std::exception`.

### 5.1 EVERY PUBLIC NAME IN `namespace stratum` (+ `using` cheat sheet)

Everything below lives in `namespace stratum` (core.hpp / stratum.hpp / reflect.hpp). You can write `stratum::X` everywhere (safest), or import what you use:

```cpp
#include <stratum/stratum.hpp>        // core.hpp + Activity + app-level API
#include <stratum/reflect.hpp>        // only if you really need reflection

using stratum::Object;                // base of every generated class (RAII global ref)
using stratum::Activity;              // = stratum::android::app::Activity
using stratum::JavaException;         // thrown by every failed Java call
using stratum::JStr;                  // String argument view  (usually NOT named: "lit"/std::string convert implicitly)
using stratum::Bool;                  // boolean argument guard (only when overload resolution picks the wrong one)
using stratum::StrArr;                // String[] argument         (non-copyable: pass as a temporary)
using stratum::ObjArr;                // Object[]/View[] argument  (non-copyable)
using stratum::ListArg;               // java.util.List/Collection argument (non-copyable)
using stratum::MapArg;                // java.util.Map argument    (non-copyable)
using stratum::Arr;                   // primitive array argument: stratum::Arr<int32_t>{1,2,3}
using stratum::Handlers;              // multi-method listener builder
using stratum::Args;                  // raw callback args: [](Args a){ a[0].obj(); }
using stratum::CbArg;                 // one callback argument (what a lambda param is converted from)
using stratum::CustomCanvasView;      // custom drawing/touch view base class
namespace reflect = stratum::reflect; // reflect::call_static / call_method / new_instance
```
Do not write `using namespace stratum;` in headers; in a `.cpp` it is fine but `Object`/`Activity` may then collide with your own names.
`JStr` and friends are **argument views**: build them inline in the call expression, never store them in a member/variable that outlives the call
(`JStr` points at the caller's `std::string` memory; `StrArr/ObjArr/ListArg/MapArg` cannot even be copied).

| Group | Names |
|---|---|
| Core object | `Object` (`adopt, raw, release, is_null, operator bool, ==, !=, to_string, hash_code, is_instance_of<T>, as<T>, unchecked_as<T>, as_array<T>, as_map`) |
| Unbox a boxed `Object` | `o.as_bool() as_i8() as_i16() as_char() as_i32() as_i64() as_float() as_double()` (throw `JavaException` if the Java type does not match) |
| Errors | `JavaException`, `check(int32_t)`, `to_std(StratumStr)` (internal helpers; you rarely call them) |
| Box | `box_bool box_i8 box_i16 box_char box_i32 box_i64 box_f32 box_f64` (+ generic `box(kind, bits)`) |
| Create | `java_string(sv)`, `new_array<T>(...)`, `new_string_array(vec)`, `new_object_array(cls, vec)`, `new_list(vec)`, `new_map(vec_of_pairs)` |
| Arg views | `JStr Bool Arr<T> StrArr ObjArr ListArg MapArg` |
| Callbacks | `Handlers`, `Args`, `CbArg`; internal `CbHolder`, `make_callback` |
| App | `Activity, getActivity, setContentView, run_on_ui_thread, register_function, register_callback, unregister_prefix, create_view, on_create/on_resume/on_pause/on_stop/on_destroy/on_back_pressed, on_broadcast, on_service_command, set_log_enabled` |
| Buffers / native | `allocate_direct_buffer, direct_buffer_address, direct_buffer_capacity, bytebuffer_data, surface_to_native_window, release_native_window` |
| Custom view | `CustomCanvasView` (`on_draw, on_measure, on_layout, on_touch_event`) |
| Reflection | `reflect::call_static<R>, reflect::call_method<R>, reflect::new_instance` |

**Callback argument object `CbArg`** (what each lambda parameter / `Args[i]` is):
`a.is_null() a.obj() a.i() a.d() a.b() a.str() a.as<T>()`; it converts implicitly to `bool`, any integer, `float/double`, `std::string`, `Object`, or any generated class (checked `as<T>`). `Args` has `size()` and `operator[]`.
**Lambda return conversion**: `void`, `bool`, integers, `float/double`, `std::string`/`const char*`, `Object`-derived, `std::vector<int32_t>`.

**Exact constructor sets (what compiles):**
| View | Accepts |
|---|---|
| `JStr` | `nullptr`, `const char*`, `const std::string&`, `std::string_view` |
| `Arr<T>` | `nullptr`, `const std::vector<T>&`, `std::initializer_list<T>` (exact `T`), `(const T*, size_t)` |
| `StrArr` | `nullptr`, `const std::vector<std::string>&`, `std::initializer_list<const char*>` (NOT a list of `std::string`) |
| `ObjArr` | `nullptr`, `std::vector<Object>`, `{Object,...}` |
| `ListArg` | `nullptr`, an existing `Object` (a Java List), `std::vector<Object>`, `{Object,...}` |
| `MapArg` | `nullptr`, an existing `Object` (a Java Map), `std::vector<std::pair<Object,Object>>` |

**Reflect argument types** (smaller set than typed calls): `bool, int8/16/32/64, char16_t/uint16_t, float, double, nullptr, Object-derived, std::string/const char*/string_view, std::vector<T>` (-> Java array). `JStr`/`Bool`/`StrArr` are NOT accepted there; a plain `char`/`int` literal is sent as `int` (use `int64_t` for a Java `long`, `int16_t` for `short`).
Overload matching is by Java parameter types, so pass the exact C++ type that maps to the Java parameter.

---

## 6. CALLBACKS

### 6.1 Single-method listener → plain lambda (tag `p`, or `a` with one method)
```cpp
btn.setOnClickListener([]() { ... });                       // 0 args
btn.setOnClickListener([label](stratum::Object v) { ... }); // typed args
btn.setOnClickListener([](stratum::Args a) { auto v = a[0].obj(); });   // raw
```
Lambdas take **0–8 args** (or one `stratum::Args`). Allowed parameter types (convert from the Java value): `bool`, any integer, `float/double`, `std::string`, `stratum::Object`, any generated class (checked `as<T>`). Take them **by value**; avoid `auto`/variadic params.
Allowed return types: `void`, `bool`, integers, `float/double`, `std::string`/`const char*`, `Object`-derived, `std::vector<int32_t>`.

### 6.2 Multi-method listener / abstract class → `Handlers` (tag `a`)
```cpp
view.setSurfaceTextureListener(
    stratum::Handlers()
        .on("onSurfaceTextureAvailable", [](stratum::Object st, int32_t w, int32_t h) { ... })
        .on("onSurfaceTextureSizeChanged", [](stratum::Object, int32_t, int32_t) {})
        .on("onSurfaceTextureDestroyed",  [](stratum::Object) -> bool { return true; })
        .on("onSurfaceTextureUpdated",    [](stratum::Object) {}));
```
- Key = the Java method **name** (overloads with the same name share one handler; branch on arg count via `stratum::Args`).
- A method with no handler does nothing and returns `false/0/null`.
- Return values are converted for Java (`boolean` accepts `bool`/non-zero int; `char` accepts char or 1-char string).
- **Do not pass a plain lambda to a multi-method listener**: it would be called for *every* method.

### 6.3 Passing an existing Java listener object
Pass the `stratum::Object` itself instead of a lambda; it is used as-is.

### 6.4 Lifetime
The closure is owned by the Java adapter/proxy and freed when Java GCs it (`finalize` → `nativeRelease`). Objects captured by value hold global refs until then; capturing the very view that owns the listener forms a cycle (fine for app-lifetime UI, a leak for transient views). Callbacks may arrive on any thread; the engine attaches/detaches threads for you.

### 6.5 `guard`
```cpp
template <class F> static void guard(F&& f) noexcept {
    try { f(); } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR, "app", "%s", e.what()); }
}
btn.setOnClickListener([]() { guard([&]{ /* work */ }); });
```

---

## 7. APP-LEVEL API (`<stratum/stratum.hpp>`)

```cpp
using stratum::Activity;                              // = stratum::android::app::Activity

// Lifecycle — define in the GLOBAL namespace (weak symbols, picked up automatically):
void onCreate(Activity act);     // or: void onCreate();
void onResume(); void onPause(); void onStop(); void onDestroy();
bool onBackPressed();            // return true = consumed
// or register at runtime: stratum::on_create([](Activity a){}); on_resume/on_pause/on_stop/on_destroy([]{}); on_back_pressed([]{return false;});

stratum::getActivity();                           // Activity (new ref), null before onCreate
stratum::setContentView(view);                    // uses current Activity (UI thread)
stratum::setContentView(act, view);               // (act.setContentView(view) also works)
stratum::run_on_ui_thread([=]() { ... });
stratum::register_function("name", lambda);       // callable from Java: StratumRuntimeLookup.callStratum("name", args...)
stratum::register_callback("Key#method", lambda); // raw key (StratumService#onCreate, ...)
stratum::unregister_prefix("Key#");
stratum::create_view("key");                      // StratumView; callbacks "<key>#onDraw|onMeasure|onLayout|onTouchEvent"
stratum::on_broadcast([](Object ctx, Object intent){});          // StratumReceiver
stratum::on_service_command([](Object intent, int32_t flags, int32_t startId) -> int32_t { return 2; });
stratum::allocate_direct_buffer(n); direct_buffer_address(obj); direct_buffer_capacity(obj); bytebuffer_data(obj);
stratum::surface_to_native_window(surface); release_native_window(w);   // ANativeWindow
stratum::set_log_enabled(true);
```
Include `<stratum/stratum.hpp>` in the translation unit that defines the lifecycle functions. The `libstratum.so`/headers hash is verified on load (abort on mismatch; define `STRATUM_SKIP_HASH_CHECK` to disable).

**Custom drawing / touch view (no Java needed)**
```cpp
class MyView : public stratum::CustomCanvasView {      // non-copyable; keep at a stable address (unique_ptr / static)
public:
    void on_draw(stratum::Object canvas) override {...}
    std::vector<int32_t> on_measure(int32_t wSpec, int32_t hSpec) override { return {}; }  // {w,h} sets measured size
    void on_layout(bool changed, int32_t l, int32_t t, int32_t r, int32_t b) override {}    // override if you add child views
    bool on_touch_event(stratum::Object motionEvent) override { return true; }
};
// It IS an Object (the underlying StratumView extends ViewGroup):  View v = myView.unchecked_as<View>(); v.invalidate();
```
`StratumService`/`StratumReceiver` only reach C++ if the process already loaded your library (e.g. an Activity ran first).

---

## 8. JAVA vs STRATUM C++ — SIDE BY SIDE

### 8.1 UI
```java
LinearLayout root = new LinearLayout(this);
root.setOrientation(LinearLayout.VERTICAL);
TextView tv = new TextView(this);
tv.setText("Hello"); tv.setTextSize(24f); tv.setTextColor(0xFF00E5FF);
root.addView(tv);
LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, 0, 1f);
lp.setMargins(0, 20, 0, 20);
root.addView(child, lp);
setContentView(root);
```
```cpp
#include <stratum/stratum.hpp>
#include <stratum/android/widget/LinearLayout.hpp>
#include <stratum/android/widget/LinearLayout_LayoutParams.hpp>
#include <stratum/android/widget/TextView.hpp>
using namespace stratum::android::widget;
static inline int32_t C(uint32_t c) { return static_cast<int32_t>(c); }

void onCreate(stratum::Activity act) {
    LinearLayout root(act);
    root.setOrientation(LinearLayout::VERTICAL);
    TextView tv(act);
    tv.setText("Hello"); tv.setTextSize(24.0f); tv.setTextColor(C(0xFF00E5FF));
    root.addView(tv);
    LinearLayout_LayoutParams lp(-1, 0, 1.0f);
    lp.setMargins(0, 20, 0, 20);
    root.addView(child, lp);
    stratum::setContentView(act, root);
}
```

### 8.2 No-arg constructor vs null (RULE 1)
```java
Paint p = new Paint(); p.setColor(Color.RED);
```
```cpp
Paint p = Paint::create();       // NOT `Paint p;`  (that is a NULL object)
p.setColor(C(0xFFFF0000));
```

### 8.3 Services, downcast, strings back
```java
SensorManager sm = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
String s = tv.getText().toString();
View v = findViewById(1001); Button b = (Button) v;
```
```cpp
SensorManager sm = act.getSystemService("sensor").as<SensorManager>();
std::string s = tv.getText();                       // CharSequence return => std::string already
Object v = act.findViewById(1001);  Button b = v.as<Button>();
```

### 8.4 Constants and fields
```java
int vis = View.GONE;  String rel = Build.VERSION.RELEASE;  int sdk = Build.VERSION.SDK_INT;
float[] vals = event.values;  lp.width = 100;
```
```cpp
int32_t vis = View::GONE;                                   // constexpr
std::string rel = Build_VERSION::sf_get_RELEASE();          // header: <stratum/android/os/Build_VERSION.hpp>
int32_t sdk = Build_VERSION::sf_get_SDK_INT();
std::vector<float> vals = event.f_get_values();
lp.f_set_width(100);
```

### 8.5 Arrays / collections
```java
canvas.drawLines(new float[]{0,0,10,10}, paint);
Arrays.deepToString(new Object[]{ "a", 1 });
```
```cpp
canvas.drawLines(std::vector<float>{0,0,10,10}, paint);       // or {0.f,0.f,10.f,10.f}
Arrays::deepToString(std::vector<stratum::Object>{ stratum::java_string("a"), stratum::box_i32(1) });
```

### 8.6 Single listener, multi listener
```java
btn.setOnClickListener(v -> toast("hi"));
seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
    public void onProgressChanged(SeekBar s, int p, boolean user) { ... }
    public void onStartTrackingTouch(SeekBar s) {}
    public void onStopTrackingTouch(SeekBar s) {}
});
webView.setWebViewClient(new WebViewClient() {
    public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) { return false; }
    public void onPageFinished(WebView v, String url) { ... }
});
```
```cpp
btn.setOnClickListener([]() { guard([&]{ toast("hi"); }); });
seek.setOnSeekBarChangeListener(stratum::Handlers()
    .on("onProgressChanged", [](stratum::Object, int32_t p, bool user) { guard([&]{ ... }); })
    .on("onStartTrackingTouch", [](stratum::Object) {})
    .on("onStopTrackingTouch",  [](stratum::Object) {}));
webView.setWebViewClient(stratum::Handlers()
    .on("shouldOverrideUrlLoading", [](stratum::Object, stratum::Object) -> bool { return false; })
    .on("onPageFinished", [](stratum::Object, std::string url) { ... }));
```

### 8.7 Threads
```java
new Thread(() -> { String r = work(); runOnUiThread(() -> tv.setText(r)); }).start();
```
```cpp
std::thread([tv]() {
    std::string r = work();
    stratum::run_on_ui_thread([tv, r]() { tv.setText(r); });
}).detach();
```

### 8.8 Toast / Intents
```java
Toast.makeText(this, "Hi", Toast.LENGTH_SHORT).show();
Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse("https://x.com")); startActivity(i);
```
```cpp
Toast::makeText(act, "Hi", Toast::LENGTH_SHORT).unchecked_as<Toast>().show();   // makeText returns Object
// Intent(String, Uri): Uri::parse returns Object
Intent i("android.intent.action.VIEW", Uri::parse("https://x.com"));
act.startActivity(i);
```

### 8.9 Camera2 (multi-callback + collections) — adapted from the working demo
```cpp
CameraManager cm = act.getSystemService("camera").as<CameraManager>();
std::vector<std::string> ids = cm.getCameraIdList();
cm.openCamera(ids[0], stratum::Handlers()
    .on("onOpened", [surface](Object dev) {
        g_dev = dev.as<CameraDevice>();
        g_dev.createCaptureSession(stratum::new_list({ surface }),
            stratum::Handlers().on("onConfigured", [surface](Object s) {
                g_session = s.as<CameraCaptureSession>();
                CaptureRequest_Builder b = g_dev.createCaptureRequest(CameraDevice::TEMPLATE_PREVIEW).as<CaptureRequest_Builder>();
                b.addTarget(surface);
                g_session.setRepeatingRequest(b.build().as<CaptureRequest>(), Object(), Object());
            }), Object());
    })
    .on("onDisconnected", [](Object) { stop_camera(); })
    .on("onError", [](Object, int32_t err) { stop_camera(); }),
    Object());      // Handler = null (callbacks on the calling thread's looper)
```
(`Surface surface(surfaceTexture);` — constructor taking an `Object`.)

---

## 9. REFLECTION (`<stratum/reflect.hpp>`) — WHEN, WHY, AND THE PERMISSION QUESTION

### 9.1 It is NOT about security
Android permission checks happen in system services (Binder), not in your JNI/reflection layer. A typed Stratum call,
a reflective call and plain Java are identical to the OS. **Reflection is never needed "for security".**

### 9.2 Why reflection exists
Generated headers cover only what is in `android.jar` (the platform). Everything else has no header:
- `androidx.*` / Jetpack / Material (`ActivityCompat`, `ContextCompat`, `RecyclerView`, …) — compiled into your APK by Gradle.
- Third-party SDKs (Firebase, Play Services, AdMob…).
- **Your own Java classes** in `src/main/java`.
- Hidden / blocked platform APIs (stripped; on Android 9+ may also fail reflectively).

For those, use `stratum::reflect` (name-based, slower, no compile-time check, **public members only**, overloads matched by argument types).

### 9.2b PERFORMANCE: TYPED FIRST, REFLECT ONLY WHEN NEEDED
| | Typed headers (`TextView::setText`) | `stratum::reflect` / `stratum_reflect_call` |
|---|---|---|
| Lookup | precomputed `(class_id, slot)`, jmethodID cached after first call | class + method found **by name** and overloads scored from argument types |
| Cost per call | ~ one JNI call | extra string lookups + overload resolution each call |
| Compile-time checking | yes (wrong name/arg = compile error) | none (errors at runtime) |
| Works for | everything in `android.jar` | anything on the classpath (androidx, 3rd-party, your classes) |

**Rule:** if a `<stratum/android/...>` header exists for the class, ALWAYS use it. Never use reflect for `android.jar` classes "to be safe", and never inside hot loops (per-frame, per-touch, per-sample).
If you must call a reflective target repeatedly, create the object once (`new_instance`) and keep it; or write one small Java helper method that does the whole job and call it once.

```cpp
#include <stratum/reflect.hpp>
// static:   R call_static<R>(className, method, args...)
// instance: R call_method<R>(targetObject, method, args...)      <- free function, NOT a member of Object
// ctor:     Object new_instance(className, args...)
int32_t st = stratum::reflect::call_static<int32_t>("androidx.core.content.ContextCompat", "checkSelfPermission", act, std::string("android.permission.CAMERA"));
stratum::reflect::call_static<void>("androidx.core.app.ActivityCompat", "requestPermissions", act, std::vector<std::string>{"android.permission.CAMERA"}, 1001);
Object helper = stratum::reflect::new_instance("com.example.Tracker", act, "KEY");
stratum::reflect::call_method<void>(helper, "log", "event");
```
Argument types accepted: `bool`, ints, `float/double`, `nullptr`, `Object`-derived, `std::string`/`const char*`/`string_view`, and `std::vector<T>` (turned into a Java array: `vector<string>`→`String[]`, `vector<int>`→`int[]`, `vector<Object>`→`Object[]`). Result `R`: `void, bool, int types, float/double, std::string, Object-derived`.

### 9.3 What really needs Java: *receiving* results from methods the Activity itself must override
Native code cannot override `Activity` methods. `StratumActivity` only forwards `onCreate/onResume/onPause/onStop/onDestroy/onBackPressed`.
So `onRequestPermissionsResult`, `onActivityResult`, `onNewIntent`, `onKeyDown`, `onConfigurationChanged`… need a small Java override in `MainActivity`
that forwards into C++ with **`StratumRuntimeLookup.callStratum(name, args...)`**, received by **`stratum::register_function(name, lambda)`**.

**Requesting a permission (all-native request + Java forward for the result)**

`MainActivity.java`
```java
@Override
public void onRequestPermissionsResult(int rc, String[] perms, int[] results) {
    super.onRequestPermissionsResult(rc, perms, results);
    boolean ok = results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED;
    com.stratum.runtime.StratumRuntimeLookup.callStratum("onPermissionResult", rc, ok);
}
```
`main.cpp`
```cpp
#include <stratum/android/content/pm/PackageManager.hpp>
using stratum::android::content::pm::PackageManager;

void onCreate(stratum::Activity act) {
    stratum::register_function("onPermissionResult", [act](int32_t rc, bool granted) {
        guard([&]{ if (granted) start_camera(); else toast("denied"); });
    });
    // Both are android.jar API 23+: typed, NO reflection needed
    if (act.checkSelfPermission("android.permission.CAMERA") != PackageManager::PERMISSION_GRANTED)
        act.requestPermissions(stratum::StrArr{"android.permission.CAMERA"}, 1001);
    else start_camera();
}
```
Remember: the `<uses-permission>` must still be in `AndroidManifest.xml`.

### 9.4 Decision table
| Need | Use |
|---|---|
| Class/method in `android.jar` | typed headers |
| `androidx`, 3rd-party, your Java class (call it) | `stratum::reflect` |
| React to something only a Java override can receive (activity result, permission result, key events) | tiny Java override → `callStratum` → `register_function` |
| Subclass a class without a generated adapter, or pass a functional interface whose param is tag `L` (e.g. `Executor`, `Consumer`) | small Java helper class in the app, call it via `reflect` |
| Manifest entries, resources, Gradle deps | normal Android files |

---

## 10. C ABI (`stratum_c_abi.h`) — SHORT GUIDE

Everything in C++ is a thin layer over this stable C ABI (`extern "C"`, symbols exported from `libstratum.so`).

```c
int64_t stratum_new_instance(cid, slot, const StratumArg* a, int32_t* err);
void    stratum_call_v(int64_t obj, cid, slot, a, &err);           // also _z (bool) _i (byte/char/short/int) _j _d (float/double)
StratumStr stratum_call_str(...);  int64_t stratum_call_o(...);    // object -> new global ref (you own it)
void    stratum_call_arr(..., StratumArr* out, &err);  stratum_arr_free(&out);
stratum_field_get_*/set_*;  stratum_delete_ref(h);  stratum_clone_ref(h);  stratum_new_string(&str);
stratum_box/unbox, new_list/new_map/new_array/new_object_array, stratum_register_callback(key,&desc),
stratum_post_ui(&desc), stratum_set_lifecycle_dispatcher(fn), stratum_get_activity(), stratum_create_view(key)
int32_t stratum_reflect_call(className, target, method, const StratumVal* args, argc, StratumVal* out);  // 0 ok, 1 error
```
- Handles are `int64_t` JNI **global refs**. Every `err`-taking function sets `*err` = 0/1; text via `stratum_last_error()`.
- `StratumArg` is a union: `.i` (ints, bool, object handle), `.f`, `.d`, `.p` (pointer to `StratumStr`, `StratumSlice`, `StratumListArg`, `StratumMapArg`, `StratumCbDesc` depending on the tag).
- **`(cid, slot)` numbers come from the resolved JSON (`class_id`, method `slot`) of the exact pipeline run; there is no C header with them and they change if the class set changes.** For hand-written C, prefer `stratum_reflect_call` (by name, no ids).
- Callbacks: fill a `StratumCbDesc{userdata, fn, drop, existing}`; `fn(userdata, methodName, StratumVal* args, argc, StratumVal* out)`. Arg `OBJ` values are LOCAL refs valid only during the call (`stratum_clone_ref` to keep). A returned `OBJ` must be a global ref (engine takes ownership).

**Pure-C app skeleton** (no C++; uses reflection by name)
```c
#include <stratum_c_abi.h>
#include <string.h>
#include <android/log.h>

static int32_t life(const char* ev, int64_t act) {          // `act` is borrowed: do not delete
    if (strcmp(ev, "onCreate") != 0) return 0;
    StratumVal a[1], out; memset(a, 0, sizeof a);
    a[0].tag = STRATUM_V_OBJ; a[0].v.i = act;
    if (stratum_reflect_call("android.widget.TextView", 0, NULL, a, 1, &out)) {      // method==NULL -> constructor
        __android_log_print(ANDROID_LOG_ERROR, "app", "%s", stratum_last_error()); return 0; }
    int64_t tv = out.v.i;                                                             // owned global ref
    static const char msg[] = "Hello from pure C";
    a[0].tag = STRATUM_V_STR; a[0].v.s.ptr = msg; a[0].v.s.len = sizeof msg - 1;
    stratum_reflect_call(NULL, tv, "setText", a, 1, &out);
    stratum_set_content_view(act, tv);
    stratum_delete_ref(tv);
    return 0;
}
__attribute__((constructor)) static void init(void) { stratum_set_lifecycle_dispatcher(life); }  // runs at System.loadLibrary
```
Lifecycle events: `onCreate onResume onPause onStop onDestroy`.

---

## 11. GOTCHAS & KNOWN LIMITS

| Symptom / situation | Cause → Fix |
|---|---|
| `JavaException: call on null Java object: ...#method` | You used `Cls x;` → use `Cls::create()` or a real constructor / check for null returns. |
| Compile: ambiguous overload on colors/ints | Literal is `unsigned`/wrong width → `C(0xFF...)`, or cast to `int32_t`/`int64_t` explicitly. |
| Compile: `"text"` picks a `boolean` overload (e.g. `put(String, String)` vs `put(String, Boolean)`) | Pass `stratum::JStr("text")` / `stratum::Bool(true)`. `Bool` only accepts a real `bool`. |
| Compile: cannot pass lambda | That param is tag `L` (`const Object&`), e.g. un-adapted functional interface. Check the generated header / `API_INDEX.txt`: callback params appear as template `C0&&`. Use a Java helper (section 9.4). |
| Callback fires for the wrong method / many times | You passed a plain lambda to a multi-method listener → use `Handlers`. |
| App crashes after a callback | C++ exception escaped → `guard`. |
| `JavaException: class not found on this device` | Class not on this Android version, or R8 stripped adapters → keep rules; check API level. |
| `method unavailable on this device API level` (only logged, then thrown on call) | Method newer than device. Guard with `Build_VERSION::sf_get_SDK_INT()`. |
| Abort "libstratum.so and SDK headers were generated from different inputs" | Rebuild both from the same pipeline output (same zip). |
| `Duplicate class com.stratum...` | Java runtime added twice (srcDir **and** module). Use one. |
| Static method not found on subclass | Statics are generated only on the declaring class: call `Parent::method()`. |
| Inherited constants missing | Constants come from the class and its superclass chain, **not from interfaces** — use the interface's own class (e.g. `Interface::CONST`). |
| `std::runtime_error: object is not an instance of X` | Wrong `.as<X>()` target. |
| Camera/sensor/etc. "does nothing" | Missing manifest `<uses-permission>` or runtime permission not granted. |
| Listener leaks / never freed | Closure freed only when Java GCs the adapter; avoid capturing the owning view by value for transient views. |
| Compile: `no matching function for call to object of type 'const (lambda ...)'` / `method is not marked const` | A `mutable` lambda is called through a const copy. Remove `mutable` (Object methods are const) or use `std::function` / make the caller `mutable` (section 3.1). |
| Compile: `character too large for enclosing character literal type` (`'×'`, `'÷'`) | Non-ASCII in a `char` literal. Use ASCII `char` codes for logic and a UTF-8 `"×"` string only for labels (section 3.2). |
| Slow UI / janky loop using `stratum::reflect` | Reflection resolves by name per call. Use the typed header if the class is in `android.jar`; otherwise hoist objects out of the loop or use a Java helper. |
| `Object` strings | A Java `String` held as `Object` cannot be passed as `JStr`; use `obj.to_string()` to get `std::string`. |
| Only some APIs exist | Private + hidden-API-blocked members are stripped (`04_5_sanitize`); androidx/third-party are never generated. |

---

## 12. LLM OUTPUT CHECKLIST

Before answering, verify:
- [ ] Every class used has its `#include <stratum/android/.../Class.hpp>` (nested: `Outer_Inner.hpp`) and a `using`.
- [ ] No `Cls x;` where a live object is needed (use `create()` / constructor).
- [ ] `JStr` for Strings in, `std::string` out; `Handlers` for multi-method callbacks; `guard` in callbacks.
- [ ] Object returns downcast with `.as<T>()`; colors via `C()`.
- [ ] No non-ASCII `char` literals (`'×'`); logic uses ASCII, labels use UTF-8 strings.
- [ ] State kept in a struct (shared_ptr/static), not read back from views; digits tested with `isdigit`, not string `<`/`>`.
- [ ] No `mutable` lambdas unless a by-value capture is truly modified; no `[&]` in deferred callbacks.
- [ ] UI work off-thread goes through `run_on_ui_thread`.
- [ ] Typed API wherever a header exists (faster); `reflect` only for non-`android.jar` classes; Java glue (+ `callStratum`/`register_function`) for results only a Java override receives.
- [ ] Mention required Manifest permissions / Java / Gradle changes.
- [ ] If an API's exact C++ signature is uncertain: say so and ask the user to grep `API_INDEX.txt`.

---

## 13. COMPLETE MINIMAL APP

`main.cpp`
```cpp
#include <stratum/stratum.hpp>
#include <stratum/android/widget/LinearLayout.hpp>
#include <stratum/android/widget/TextView.hpp>
#include <stratum/android/widget/Button.hpp>
#include <stratum/android/widget/Toast.hpp>
#include <android/log.h>
#include <string>
using namespace stratum::android::widget;

static inline int32_t C(uint32_t c) { return static_cast<int32_t>(c); }
template <class F> static void guard(F&& f) noexcept {
    try { f(); } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR, "app", "%s", e.what()); }
}

void onCreate(stratum::Activity act) {
    static int counter = 0;

    LinearLayout root(act);
    root.setOrientation(LinearLayout::VERTICAL);
    root.setBackgroundColor(C(0xFF101418));
    root.setPadding(32, 64, 32, 32);

    TextView label(act);
    label.setText("Counter: 0");
    label.setTextSize(26.0f);
    label.setTextColor(C(0xFFFFFFFF));
    root.addView(label);

    Button btn(act);
    btn.setText("Click me");
    btn.setOnClickListener([act, label]() {
        guard([&] {
            label.setText("Counter: " + std::to_string(++counter));
            Toast::makeText(act, "Clicked", Toast::LENGTH_SHORT).unchecked_as<Toast>().show();
        });
    });
    root.addView(btn);

    stratum::setContentView(act, root);
}

// optional: bool onBackPressed() { return false; }
```
`MainActivity.java`: see section 2. `CMakeLists.txt`: see section 2.

---

## 14. CLEAN IDIOMATIC EXAMPLE (calculator) — copy this structure

One state struct, ONE handler function, data-driven key table, ASCII logic codes + UTF-8 labels, non-mutable lambdas, `guard` around callbacks.

```cpp
#include <stratum/stratum.hpp>
#include <stratum/android/widget/LinearLayout.hpp>
#include <stratum/android/widget/LinearLayout_LayoutParams.hpp>
#include <stratum/android/widget/TextView.hpp>
#include <stratum/android/widget/Button.hpp>
#include <stratum/android/view/Gravity.hpp>
#include <android/log.h>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using stratum::android::widget::LinearLayout;
using stratum::android::widget::LinearLayout_LayoutParams;
using stratum::android::widget::TextView;
using stratum::android::widget::Button;
using stratum::android::view::Gravity;

static inline int32_t C(uint32_t c) { return static_cast<int32_t>(c); }
template <class F> static void guard(F&& f) noexcept {
    try { f(); } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR, "calc", "%s", e.what()); }
}

struct Key { const char* label; const char* id; };   // label: shown (UTF-8). id: ASCII logic code.
struct Calc {
    std::string text = "0";
    double acc = 0.0;
    char op = 0;               // '+', '-', '*', '/'  (ASCII only!)
    bool fresh = true;
};

static std::string fmt(double v) {
    if (std::isnan(v)) return "Error";
    std::ostringstream ss; ss.precision(10); ss << v; return ss.str();
}
static double apply(char op, double a, double b) {
    switch (op) { case '+': return a + b; case '-': return a - b; case '*': return a * b;
                  case '/': return b != 0.0 ? a / b : NAN; default: return b; }
}

static void press(const std::string& id, Calc& c, const TextView& display) {
    if (id.size() == 1 && std::isdigit(static_cast<unsigned char>(id[0]))) {
        if (c.fresh || c.text == "0") c.text = id; else if (c.text.size() < 12) c.text += id;
        c.fresh = false;
    } else if (id == ".") {
        if (c.fresh) { c.text = "0."; c.fresh = false; }
        else if (c.text.find('.') == std::string::npos) c.text += ".";
    } else if (id == "C") {
        c = Calc();
    } else if (id == "+" || id == "-" || id == "*" || id == "/") {
        double cur = std::stod(c.text);
        if (c.op && !c.fresh) { c.acc = apply(c.op, c.acc, cur); c.text = fmt(c.acc); } else c.acc = cur;
        c.op = id[0]; c.fresh = true;
    } else if (id == "=") {
        if (c.op) { c.text = fmt(apply(c.op, c.acc, std::stod(c.text))); c.op = 0; c.fresh = true; }
    }
    display.setText(c.text);
}

void onCreate(stratum::Activity act) {
    guard([&] {
        auto st = std::make_shared<Calc>();

        LinearLayout root(act);
        root.setOrientation(LinearLayout::VERTICAL);
        root.setBackgroundColor(C(0xFF121212));
        root.setPadding(24, 48, 24, 24);

        TextView display(act);
        display.setText("0");
        display.setTextSize(48.0f);
        display.setTextColor(C(0xFFFFFFFF));
        display.setGravity(Gravity::RIGHT | Gravity::BOTTOM);
        root.addView(display, LinearLayout_LayoutParams(-1, 0, 1.5f));

        const std::vector<std::vector<Key>> grid = {
            {{"C","C"},{"7","7"},{"8","8"},{"÷","/"}},
            {{"4","4"},{"5","5"},{"6","6"},{"×","*"}},
            {{"1","1"},{"2","2"},{"3","3"},{"-","-"}},
            {{"0","0"},{".","."},{"=","="},{"+","+"}},
        };

        for (const auto& rowKeys : grid) {
            LinearLayout row(act);
            row.setOrientation(LinearLayout::HORIZONTAL);
            for (const Key& k : rowKeys) {
                Button b(act);
                b.setText(k.label);
                b.setTextSize(22.0f);
                b.setBackgroundColor(C(0xFF333333));
                b.setTextColor(C(0xFFFFFFFF));
                std::string id = k.id;
                b.setOnClickListener([id, st, display]() {          // NOT mutable
                    guard([&] { press(id, *st, display); });
                });
                row.addView(b, LinearLayout_LayoutParams(0, -1, 1.0f));
            }
            root.addView(row, LinearLayout_LayoutParams(-1, 0, 1.0f));
        }
        stratum::setContentView(act, root);
    });
}
```
Why this is clean: no `mutable`, no wide `char` literals, no per-click string reparsing of the view, one shared state object, the click lambda only forwards to a plain function. (Unverified against a compiler here; if a header signature differs, check `API_INDEX.txt`.)