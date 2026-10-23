cmake_minimum_required(VERSION 3.18)
project(stratum_engine CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED STRATUM_LOG_ENABLED)
  set(STRATUM_LOG_ENABLED 0)
endif()

add_library(stratum SHARED            # -> libstratum.so  (what StratumActivity loads)
    src/bridge_core.cpp src/stratum_engine.cpp src/engine_main.cpp src/stratum_reflect.cpp
    generated/metadata_table.cpp)
# PUBLIC (absolute path): anything that links `stratum` automatically gets
# <stratum/...> headers + stratum_c_abi.h. Required for add_subdirectory use.
target_include_directories(stratum PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(stratum PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                           ${CMAKE_CURRENT_SOURCE_DIR}/generated)
target_compile_definitions(stratum PRIVATE STRATUM_LOG_ENABLED=${STRATUM_LOG_ENABLED})
# Anything that links `stratum` (your app lib) compiles the header-only wrappers at -O2 even in Debug.
target_compile_options(stratum INTERFACE $<$<CONFIG:Debug>:-O2>)
target_compile_options(stratum PRIVATE -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -Wno-unused-parameter)
find_library(log-lib log)
find_library(android-lib android)
target_link_libraries(stratum PRIVATE ${log-lib} ${android-lib})
target_link_options(stratum PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)