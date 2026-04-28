#!/bin/bash

echo "[Build] Compiling Cup Stack Test..."

if [ ! -d "SRC" ]; then
    echo "[Build] ERROR: SRC directory not found!"
    exit 1
fi

if [ ! -f "SRC/CAENRFIDLib_Light.c" ] || [ ! -f "SRC/host.c" ] || [ ! -f "SRC/IO_Light.c" ]; then
    echo "[Build] ERROR: Missing CAEN library files in SRC folder!"
    exit 1
fi

if [ ! -f "cup_stack_test.c" ]; then
    echo "[Build] ERROR: cup_stack_test.c not found!"
    exit 1
fi

gcc \
  cup_stack_test.c \
  SRC/host.c SRC/CAENRFIDLib_Light.c SRC/IO_Light.c \
  -ISRC \
  -o cup_stack_test \
  -lpthread -lm \
  -Wall

if [ $? -eq 0 ]; then
    echo "[Build] Success! Created cup_stack_test"
    chmod +x cup_stack_test
    echo ""
    echo "Usage:"
    echo "  ./cup_stack_test                     # 100 single + 100 stack, 3000ms scan"
    echo "  ./cup_stack_test 50 50 4000          # 50 single + 50 stack, 4000ms scan"
else
    echo "[Build] Compilation failed!"
    exit 1
fi
