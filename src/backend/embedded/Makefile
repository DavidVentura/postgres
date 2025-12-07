#-------------------------------------------------------------------------
#
# Makefile for backend/embedded
#
# src/backend/embedded/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/embedded
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	pgembedded.o \
	initdb_simple.o

# Need to include initdb source directory
override CPPFLAGS := -I$(top_srcdir)/src/bin/initdb -I$(top_srcdir)/src/interfaces/libpq $(CPPFLAGS)

include $(top_srcdir)/src/backend/common.mk
