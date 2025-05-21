#!/bin/bash

# compile_jdvout.sh

g++ main.cpp programArgs.cpp fileChecks.cpp information.cpp valueUpdater.cpp decryptFile.cpp inflateFile.cpp jdvOut.cpp fromBase64.cpp getPin.cpp -Wall -O2 -lz -lsodium -s -o jdvout

if [ $? -eq 0 ]; then
    echo "Compilation successful. Executable 'jdvout' created."
else
    echo "Compilation failed."
    exit 1
fi

