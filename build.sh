#!/bin/bash
# Build PostgreSQL static library (libpostgres_server.a)

set -e  # Exit on error

echo "==================================================================="
echo "Building PostgreSQL static library (libpostgres_server.a)"
echo "==================================================================="
echo ""

# Step 1: Build all backend objects
echo "Step 1: Building all backend objects..."
echo "-------------------------------------------------------------------"
make -C src/backend all -j$(nproc)
echo ""

# Step 2: Build the static library
echo "Step 2: Building static library..."
echo "-------------------------------------------------------------------"
make -C src/backend libpostgres_server.a
echo ""

# Step 3: Verify the library
echo "Step 3: Verification"
echo "-------------------------------------------------------------------"

LIBPATH="src/backend/libpostgres_server.a"

if [ -f "$LIBPATH" ]; then
    echo "✓ Library created: $LIBPATH"

    # Show file size
    SIZE=$(du -h "$LIBPATH" | cut -f1)
    echo "  Size: $SIZE"

    # Count objects
    OBJCOUNT=$(ar t "$LIBPATH" | wc -l)
    echo "  Objects: $OBJCOUNT"

    # Show some symbols
    echo ""
    echo "Sample exported functions:"
    nm "$LIBPATH" | grep " T " | head -10 | while read addr type name; do
        echo "  - $name"
    done

    echo ""
    echo "==================================================================="
    echo "Build successful!"
    echo "==================================================================="
    echo ""
    echo "Static library: $LIBPATH"
    echo ""
    echo "Next steps:"
    echo "1. Create embedded API wrapper (src/backend/embedded/)"
    echo "2. Build test application that links against this library"
    echo ""
else
    echo "✗ ERROR: Library not created at $LIBPATH"
    exit 1
fi
