#!/bin/bash
# Compile test_embedded against libpostgres_server.a
#
# This script compiles the embedded PostgreSQL test application

set -e

echo "====================================================================="
echo "Compiling Embedded PostgreSQL Test Application"
echo "====================================================================="
echo ""

POSTGRES_ROOT="$(pwd)"
STATIC_LIB="$POSTGRES_ROOT/src/backend/libpostgres_server.a"

if [ ! -f "$STATIC_LIB" ]; then
    echo "ERROR: Static library not found at $STATIC_LIB"
    echo "Please run: make -C src/backend libpostgres_server.a"
    exit 1
fi

echo "Step 1: Compiling embedded_stubs.c..."
echo "---------------------------------------------------------------------"

musl-gcc -static \
    -I "$POSTGRES_ROOT/src/include" \
    -c embedded_stubs.c \
    -O2 \
    -fdata-sections \
    -ffunction-sections \
    -Wl,--gc-sections \
    -o embedded_stubs.o

echo ""
echo "Step 2: Compiling test_embedded.c..."
echo "---------------------------------------------------------------------"

musl-gcc -static \
    -I "$POSTGRES_ROOT/src/include" \
    -I "$POSTGRES_ROOT/src/backend/embedded" \
    test_embedded.c \
    embedded_stubs.o \
    "$STATIC_LIB" \
    -O2 \
    -fvisibility=hidden \
    -fno-asynchronous-unwind-tables \
    -fdata-sections \
    -ffunction-sections \
    -Wl,--gc-sections \
    -o test_embedded

if [ $? -eq 0 ]; then
    echo ""
    echo "====================================================================="
    echo "Build successful!"
    echo "====================================================================="
    echo ""
    echo "Executable: ./test_embedded"
    echo ""
    echo "Usage:"
    echo "  ./test_embedded <data_directory>"
    echo ""
    echo "Example:"
    echo "  # First create a data directory with initdb:"
    echo "  initdb -D /tmp/pgdata"
    echo ""
    echo "  # Then run the test:"
    echo "  ./test_embedded /tmp/pgdata"
    echo ""
else
    echo ""
    echo "====================================================================="
    echo "Build FAILED"
    echo "====================================================================="
    exit 1
fi
