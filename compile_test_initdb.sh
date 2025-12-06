#!/bin/bash
# Compile test_initdb against libpostgres_server.a

set -e

echo "====================================================================="
echo "Compiling Embedded PostgreSQL initdb Test Application"
echo "====================================================================="
echo ""

POSTGRES_ROOT="$(pwd)"
STATIC_LIB="$POSTGRES_ROOT/src/backend/libpostgres_server.a"

if [ ! -f "$STATIC_LIB" ]; then
    echo "ERROR: Static library not found at $STATIC_LIB"
    echo "Please run: ./create_static_lib.sh"
    exit 1
fi

echo "Step 1: Compiling test_initdb.c..."
echo "---------------------------------------------------------------------"

musl-gcc -static \
    -I "$POSTGRES_ROOT/src/include" \
    -I "$POSTGRES_ROOT/src/backend/embedded" \
    test_initdb.c \
    embedded_stubs.o \
    "$STATIC_LIB" \
    -lm -lpthread \
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
    echo "  ./test_initdb /tmp/pgdata_embedded"
    echo ""
else
    echo ""
    echo "====================================================================="
    echo "Build FAILED"
    echo "====================================================================="
    exit 1
fi
