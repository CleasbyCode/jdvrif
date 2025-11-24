#!/bin/bash

# compile_jdvrif.sh

g++ -std=c++23 -O3 -march=native -pipe -Wall -Wextra -Wpedantic -DNDEBUG -s -flto=auto -fuse-linker-plugin jdvrif.cpp -lturbojpeg -lz -lsodium -o jdvrif

if [ $? -eq 0 ]; then
    echo "Compilation successful. Executable 'jdvrif' created."
else
    echo "Compilation failed."
    exit 1
fi
