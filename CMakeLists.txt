cmake_minimum_required(VERSION 2.8.0)

project(stress_memcg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)

# Defaul build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Optimization flags for GCC and Clang
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall")

# Link standard library statically
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")

# We want stack to be visible in perf
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")

add_executable(stress_memcg main.cpp)
