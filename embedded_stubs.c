/*
 * embedded_stubs.c - Stubs for symbols normally provided by main.c
 *
 * These symbols are normally in main.o, which we exclude from the static
 * library. This file provides minimal implementations for embedded use.
 */

#include "postgres.h"

#include <string.h>
#include "postmaster/postmaster.h"

/* Global program name variable */
const char *progname = "postgres_embedded";

/*
 * parse_dispatch_option - Parse dispatch option from command line
 *
 * For embedded use, we always return DISPATCH_POSTMASTER since we're
 * not being invoked via command line with dispatch options.
 */
DispatchOption
parse_dispatch_option(const char *name)
{
	static const char *const DispatchOptionNames[] =
	{
		[DISPATCH_CHECK] = "check",
		[DISPATCH_BOOT] = "boot",
		[DISPATCH_FORKCHILD] = "forkchild",
		[DISPATCH_DESCRIBE_CONFIG] = "describe-config",
		[DISPATCH_SINGLE] = "single",
	};

	for (int i = 0; i < lengthof(DispatchOptionNames); i++)
	{
		if (i == DISPATCH_FORKCHILD)
		{
#ifdef EXEC_BACKEND
			if (strncmp(DispatchOptionNames[DISPATCH_FORKCHILD], name,
						strlen(DispatchOptionNames[DISPATCH_FORKCHILD])) == 0)
				return DISPATCH_FORKCHILD;
#endif
			continue;
		}

		if (DispatchOptionNames[i] && strcmp(DispatchOptionNames[i], name) == 0)
			return (DispatchOption) i;
	}

	return DISPATCH_POSTMASTER;
}
