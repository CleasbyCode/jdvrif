#!/bin/bash

# compile_jdvin.sh

g++ main.cpp programArgs.cpp fileChecks.cpp information.cpp segmentsVec.cpp jdvIn.cpp -Wall -O2 -lz -lsodium -lturbojpeg -s -o jdvin

if [ $? -eq 0 ]; then
    echo "Compilation successful. Executable 'jdvin' created."
else
    echo "Compilation failed."
    exit 1
fi
