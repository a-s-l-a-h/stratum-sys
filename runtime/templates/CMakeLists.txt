cmake_minimum_required(VERSION 3.22.1)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(stratum)                      # builds libstratum.so

# The library name MUST match System.loadLibrary("myapp") in your Activity.
add_library(myapp SHARED native-lib.cpp)
find_library(log-lib log)
target_link_libraries(myapp PRIVATE stratum ${log-lib})