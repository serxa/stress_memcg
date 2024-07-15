#!/usr/bin/env bash

mkdir -p build
cd build

if [ "$CXX" = "" ]; then
    if which clang++ > /dev/null; then
        export CXX=clang++
    elif which clang++-19 > /dev/null; then
        export CXX=clang++-19
    elif which clang++-18 > /dev/null; then
        export CXX=clang++-18
    elif which clang++-17 > /dev/null; then
        export CXX=clang++-17
    elif which clang++-16 > /dev/null; then
        export CXX=clang++-16
    elif which clang++-15 > /dev/null; then
        export CXX=clang++-15
    elif which clang++-14 > /dev/null; then
        export CXX=clang++-14
    elif which clang++-13 > /dev/null; then
        export CXX=clang++-13
    elif which clang++-12 > /dev/null; then
        export CXX=clang++-12
    elif which clang++-11 > /dev/null; then
        export CXX=clang++-11
    else
        echo "Clang not detected"
    fi
fi

echo "CXX=$CXX"

# Uncomment to use LLVM standard library
#if echo $CXX | grep clang++ > /dev/null; then
#    export CXXFLAGS="-stdlib=libc++"
#fi

cmake ..
make

# Simpler way to build without cmake, but with dynamic linking
#mkdir -p build && $CXX main.cpp -o build/stress_memcg -O3 -pthread -fno-omit-frame-pointer
