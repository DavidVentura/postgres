/*
 * embedded_stubs.c - Stubs for symbols normally provided by main.c
 *
 * These symbols are normally in main.o, which we exclude from the static
 * library. This file provides minimal implementations for embedded use.
 */

#include "postgres.h"

#include <string.h>
#include <setjmp.h>
#include "postmaster/postmaster.h"

/* Global program name variable */
const char *progname = "postgres_embedded";

/*
 * optreset - BSD extension to getopt for resetting option parsing
 *
 * This is used by postgres.c and postmaster.c for command-line parsing,
 * but since embedded mode doesn't parse command-line arguments, this stub
 * just provides the symbol to satisfy the linker.
 */
int optreset = 0;


/*
 * parse_dispatch_option - Parse dispatch option from command line
 *
 * For embedded use, we always return DISPATCH_POSTMASTER since we're
 * not being invoked via command line with dispatch options.
 */
DispatchOption parse_dispatch_option(const char *name) {
	return 0;
}
