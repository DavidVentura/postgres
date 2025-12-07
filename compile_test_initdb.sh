#!/bin/bash
# Compile test_initdb against libpostgres_server.a

set -e

echo "====================================================================="
echo "Compiling test_initdb (In-Process Database Initialization Test)"
echo "====================================================================="
echo ""

POSTGRES_ROOT="$(pwd)"
STATIC_LIB="$POSTGRES_ROOT/src/backend/libpostgres_server.a"

if [ ! -f "$STATIC_LIB" ]; then
    echo "ERROR: Static library not found at $STATIC_LIB"
    echo "Please run: ./build.sh"
    exit 1
fi

echo "Compiling test_initdb.c..."
echo "---------------------------------------------------------------------"

musl-gcc -static \
    -I "$POSTGRES_ROOT/src/include" \
    -I "$POSTGRES_ROOT/src/backend/embedded" \
    test_initdb.c \
    embedded_stubs.o \
    "$STATIC_LIB" \
    -g \
    -O0 \
    -o test_initdb

if [ $? -eq 0 ]; then
    echo ""
    echo "====================================================================="
    echo "Build successful!"
    echo "====================================================================="
    echo ""
    echo "Executable: ./test_initdb"
    echo ""
    echo "Usage:"
    echo "  ./test_initdb <data_directory>"
    echo ""
    echo "Example:"
    echo "  # Create a new database cluster:"
    echo "  ./test_initdb /tmp/pgdata_new"
    echo ""
    echo "  # Then use it with test_embedded:"
    echo "  ./test_embedded /tmp/pgdata_new"
    echo ""
else
    echo ""
    echo "====================================================================="
    echo "Build FAILED"
    echo "====================================================================="
    exit 1
fi
