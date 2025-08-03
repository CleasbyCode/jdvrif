#!/bin/bash

# compile_jdvrif.sh

g++ -std=c++20 main.cpp programArgs.cpp fileChecks.cpp information.cpp segmentsVec.cpp -Wall -O3 -lz -lsodium -lturbojpeg -s -o jdvrif

if [ $? -eq 0 ]; then
    echo "Compilation successful. Executable 'jdvrif' created."
else
    echo "Compilation failed."
    exit 1
fi
