#!/bin/bash
set -e

cd src/backend

rm -f libpostgres_server.a

# Collect all object files (use absolute paths or glob)
find access archive backup bootstrap catalog parser commands executor embedded foreign lib libpq nodes optimizer partitioning port postmaster regex replication rewrite statistics storage tcop tsearch utils jit -name "*.o" > /tmp/all_objs.txt

# Create archive with backend objects
ar crs libpostgres_server.a $(cat /tmp/all_objs.txt)

# Extract and add objects from libpgcommon_srv.a
mkdir -p .tmp_common
cd .tmp_common
ar x ../../common/libpgcommon_srv.a
ar crs ../libpostgres_server.a *.o
cd ..
rm -rf .tmp_common

# Extract and add objects from libpgport_srv.a
mkdir -p .tmp_port
cd .tmp_port
ar x ../../port/libpgport_srv.a
ar crs ../libpostgres_server.a *.o
cd ..
rm -rf .tmp_port

# Add timezone objects
ar crs libpostgres_server.a ../timezone/localtime.o ../timezone/pgtz.o ../timezone/strftime.o

# Create index
ranlib libpostgres_server.a

echo "Library created successfully:"
ls -lh libpostgres_server.a
echo "Object count: $(ar t libpostgres_server.a | wc -l)"
