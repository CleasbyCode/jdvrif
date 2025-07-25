#!/bin/bash

# compile_jdvout.sh

g++ main.cpp programArgs.cpp fileChecks.cpp jdvOut.cpp -Wall -O2 -lz -lsodium -s -o jdvout

if [ $? -eq 0 ]; then
    echo "Compilation successful. Executable 'jdvout' created."
else
    echo "Compilation failed."
    exit 1
fi

