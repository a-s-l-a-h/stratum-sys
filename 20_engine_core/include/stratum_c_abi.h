// stratum_c_abi.h — Universal C ABI of the Stratum engine (libstratum.so).
// Tags in StratumArg slots come from Stage 05 `param_tags` (same table as Python):
//  Z B C S I J F D  primitives (value in .i / .f / .d)
//  s String/CharSequence -> .p = const StratumStr*   (ptr==NULL => null)
//  [ ] q f d b c h  primitive arrays -> .p = const StratumSlice* (p==NULL => null; ptr==NULL,len==0 => empty)
//  T String[] -> .p = StratumSlice* of StratumStr    A Object[] -> StratumSlice* of int64 handles
//  M List/Collection -> .p = StratumListArg*         N Map -> .p = StratumMapArg*
//  a / p callback (adapter / dynamic proxy) -> .p = StratumCbDesc*
//  L object handle (int64 JNI global ref) in .i
#ifndef STRATUM_C_ABI_H
#define STRATUM_C_ABI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define STRATUM_API __attribute__((visibility("default")))
#define STRATUM_ABI_VERSION 1

typedef union StratumArg { int64_t i; double d; float f; const void* p; } StratumArg;
typedef struct StratumStr   { const char* ptr; size_t len; } StratumStr;
typedef struct StratumSlice { const void* ptr;  size_t len; } StratumSlice;
typedef struct StratumListArg { int64_t existing; const int64_t* ptrs; size_t len; } StratumListArg;
typedef struct StratumMapArg  { int64_t existing; const int64_t* keys; const int64_t* vals; size_t len; } StratumMapArg;
/* Arrays returned by the engine. kind: B I J F D Z C S (primitive), T (StratumStr[]), O (int64 handles). Free with stratum_arr_free. */
typedef struct StratumArr { uint8_t kind; size_t len; void* data; } StratumArr;

enum { STRATUM_V_NULL=0, STRATUM_V_BOOL=1, STRATUM_V_BYTE=2, STRATUM_V_CHAR=3, STRATUM_V_SHORT=4,
       STRATUM_V_INT=5, STRATUM_V_LONG=6, STRATUM_V_FLOAT=7, STRATUM_V_DOUBLE=8, STRATUM_V_STR=9,
       STRATUM_V_OBJ=10, STRATUM_V_ERR=11, STRATUM_V_I32ARR=12 };

/* Tagged value: callback arguments / return values and reflection arguments / results.
   Callback args: STR points into engine memory, OBJ is a LOCAL ref valid only during the call (clone to keep).
   Callback return: OBJ must be a GLOBAL ref the engine takes over; STR/I32ARR memory must stay valid until return.
   Reflection result: OBJ is a new global ref owned by the caller; STR valid until the next call on this thread. */
typedef union StratumValPayload { int64_t i; double d; struct { const void* ptr; size_t len; } s; } StratumValPayload;
typedef struct StratumVal { uint8_t tag; StratumValPayload v; } StratumVal;

typedef void (*StratumTramp)(void* userdata, const char* method, const StratumVal* args, size_t argc, StratumVal* out);
typedef void (*StratumDropFn)(void* userdata);
/* Ownership: when passed to the engine, the engine copies the descriptor and sets caller's userdata=NULL.
   existing != 0: pass that Java object (listener) as-is instead of building an adapter/proxy. */
typedef struct StratumCbDesc { void* userdata; StratumTramp fn; StratumDropFn drop; int64_t existing; } StratumCbDesc;
/* event: onCreate onResume onPause onStop onDestroy. act = borrowed global ref of current Activity. */
typedef int32_t (*StratumLifecycleFn)(const char* event, int64_t act);

/* Every function taking `int32_t* err` sets *err=0 on success, 1 on failure (text: stratum_last_error()). */
STRATUM_API int32_t     stratum_core_init(void* java_vm);       /* idempotent; called by JNI_OnLoad */
STRATUM_API int32_t     stratum_abi_version(void);
STRATUM_API const char* stratum_meta_hash(void);
STRATUM_API const char* stratum_last_error(void);
STRATUM_API void*       stratum_get_env(void);

STRATUM_API int64_t     stratum_new_instance(uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
STRATUM_API void        stratum_call_v(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
STRATUM_API uint8_t     stratum_call_z(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
STRATUM_API int64_t     stratum_call_i(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err); /* byte/char/short/int */
STRATUM_API int64_t     stratum_call_j(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
STRATUM_API double      stratum_call_d(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err); /* float/double */
STRATUM_API StratumStr  stratum_call_str(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err); /* valid until next call on thread */
STRATUM_API int64_t     stratum_call_o(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, int32_t* err);
STRATUM_API void        stratum_call_arr(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, StratumArr* out, int32_t* err);
STRATUM_API void        stratum_call_list(int64_t p, uint32_t cid, uint32_t slot, const StratumArg* a, StratumArr* out, int32_t* err); /* Collection -> kind 'O' */

STRATUM_API int64_t     stratum_field_get_i(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API uint8_t     stratum_field_get_z(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API int64_t     stratum_field_get_j(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API double      stratum_field_get_d(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API StratumStr  stratum_field_get_str(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API int64_t     stratum_field_get_o(int64_t p, uint32_t cid, uint32_t slot, int32_t* err);
STRATUM_API void        stratum_field_get_arr(int64_t p, uint32_t cid, uint32_t slot, StratumArr* out, int32_t* err);
STRATUM_API void        stratum_field_set_i(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err);
STRATUM_API void        stratum_field_set_z(int64_t p, uint32_t cid, uint32_t slot, uint8_t v, int32_t* err);
STRATUM_API void        stratum_field_set_j(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err);
STRATUM_API void        stratum_field_set_d(int64_t p, uint32_t cid, uint32_t slot, double v, int32_t* err);
STRATUM_API void        stratum_field_set_str(int64_t p, uint32_t cid, uint32_t slot, const StratumStr* v, int32_t* err);
STRATUM_API void        stratum_field_set_o(int64_t p, uint32_t cid, uint32_t slot, int64_t v, int32_t* err);

STRATUM_API void        stratum_arr_free(StratumArr* a);
STRATUM_API void        stratum_get_array_data(int64_t arr_handle, StratumArr* out, int32_t* err);
STRATUM_API void        stratum_delete_ref(int64_t p);
STRATUM_API int64_t     stratum_clone_ref(int64_t p);
STRATUM_API uint8_t     stratum_is_instance_of(int64_t p, uint32_t class_id);
STRATUM_API uint8_t     stratum_is_same_object(int64_t a, int64_t b);
STRATUM_API int64_t     stratum_new_string(const StratumStr* s);
STRATUM_API StratumStr  stratum_to_string(int64_t p);                 /* valid until next call on thread */
STRATUM_API int32_t     stratum_hash_code(int64_t p);
STRATUM_API int64_t     stratum_box(uint8_t kind, int64_t bits);      /* kind Z B C S I J F D; F/D = raw bit pattern */
STRATUM_API int64_t     stratum_unbox(int64_t obj_handle, uint8_t kind, int32_t* err);
STRATUM_API int64_t     stratum_new_list(const int64_t* items, size_t n);
STRATUM_API int64_t     stratum_new_map(const int64_t* keys, const int64_t* vals, size_t n);
STRATUM_API void        stratum_get_map_entries(int64_t map_handle, StratumArr* keys_out, StratumArr* vals_out, int32_t* err);

STRATUM_API void        stratum_register_callback(const char* key, const StratumCbDesc* d);  /* e.g. "StratumService#onCreate", "app#fn" */
STRATUM_API void        stratum_remove_callback(const char* key);
STRATUM_API size_t      stratum_remove_callbacks_by_prefix(const char* prefix);
STRATUM_API size_t      stratum_callback_count(void);
STRATUM_API void        stratum_set_lifecycle_dispatcher(StratumLifecycleFn f);
STRATUM_API int32_t     stratum_post_ui(const StratumCbDesc* d);    /* run on main looper; returns 0 ok */

STRATUM_API int64_t     stratum_get_activity(void);                 /* new global ref, caller owns; 0 before onCreate */
STRATUM_API int64_t     stratum_create_view(const char* key);       /* com.stratum.runtime.StratumView; callbacks "<key>#onDraw|onMeasure|onLayout|onTouchEvent" */
STRATUM_API void        stratum_set_content_view(int64_t act, int64_t view);

STRATUM_API void*       stratum_get_direct_buffer_address(int64_t bb);
STRATUM_API size_t      stratum_get_direct_buffer_capacity(int64_t bb);
STRATUM_API void        stratum_get_bytebuffer_data(int64_t bb, StratumArr* out, int32_t* err);
STRATUM_API int64_t     stratum_allocate_direct_buffer(size_t capacity);
STRATUM_API void*       stratum_surface_to_native_window(int64_t surface);
STRATUM_API void        stratum_release_native_window(void* window);

/* Dynamic reflection (works on your own app classes). method==NULL -> constructor. target==0 -> static.
   Returns 0 ok, 1 error. */
STRATUM_API int32_t     stratum_reflect_call(const char* class_name, int64_t target, const char* method,
                                             const StratumVal* args, size_t argc, StratumVal* out);

STRATUM_API void        stratum_set_log_enabled(uint8_t enabled);
STRATUM_API uint8_t     stratum_is_log_enabled(void);
STRATUM_API void        stratum_log_msg(const char* msg);

#ifdef __cplusplus
}
#endif
#endif