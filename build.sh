#!/usr/bin/env bash

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make

# Simpler way to build without cmake and with dynamic linking
#mkdir -p build && clang++-11 main.cpp -o build/stress_memcg -O3 -pthread -fno-omit-frame-pointer