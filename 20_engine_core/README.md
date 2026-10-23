
# 1. engine
python 20_engine_core/emit.py  --input 05_resolve/output_patched
python 20_engine_core/build.py --abi arm64-v8a            # add --log for deep tracing; repeat for x86_64
# -> 20_engine_core/output/arm64-v8a/libstratum.so

# 2. SDK (start restricted; drop --emit-prefix later for the whole SDK)
python 21_cpp_emit/main.py --input 05_resolve/output_patched \
    --emit-prefix android.app.,android.view.,android.widget.,android.content.,android.os.,android.graphics.,java.lang.,java.util.
grep -E "Button:.*setText|View:.*setOnClickListener|ViewGroup:.*addView" 21_cpp_emit/output/include/stratum/API_INDEX.txt