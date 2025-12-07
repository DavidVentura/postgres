#!/bin/bash
# Configure PostgreSQL for static library build with musl

set -e  # Exit on error

echo "Configuring PostgreSQL for static library build with musl..."

CC=musl-gcc \
CFLAGS="-O0 -g2 -static -ffunction-sections -fdata-sections" \
LDFLAGS="-static" \
./configure \
  --prefix=/tmp/pg-embedded-install \
  --without-readline \
  --without-zlib \
  --without-icu \
  --with-openssl=no \
  --without-ldap \
  --without-pam \
  --without-gssapi \
  --without-systemd \
  --without-llvm \
  --disable-largefile \
  --disable-spinlocks \
  --enable-debug

echo ""
echo "Configuration complete! Verifying settings..."
echo ""

# Verify musl is being used
if grep -q "musl" config.log; then
    echo "✓ musl detected in config.log"
else
    echo "✗ WARNING: musl not detected in config.log"
fi

# Check LIBS
echo -n "LIBS: "
grep "^LIBS = " src/Makefile.global || echo "(not found)"

# Check LDFLAGS
echo -n "LDFLAGS: "
grep "^LDFLAGS = " src/Makefile.global || echo "(not found)"

echo ""
echo "Ready to build! Run ./build.sh to build the static library."
