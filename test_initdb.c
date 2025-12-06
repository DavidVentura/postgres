/*
 * test_initdb.c - Test pg_embedded_initdb function
 *
 * This creates a new PostgreSQL data directory using the embedded API
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "src/backend/embedded/pgembedded.h"

int main(int argc, char **argv)
{
	const char *datadir;
	struct stat st;

	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <data_directory>\n", argv[0]);
		fprintf(stderr, "Example: %s /tmp/pgdata_embedded\n", argv[0]);
		return 1;
	}

	datadir = argv[1];

	printf("========================================\n");
	printf("PostgreSQL Embedded initdb Test\n");
	printf("========================================\n\n");

	/* Check if directory already exists */
	if (stat(datadir, &st) == 0)
	{
		printf("WARNING: Directory %s already exists\n", datadir);
		printf("Please use a non-existent directory or remove the existing one.\n");
		return 1;
	}

	printf("Initializing new database in: %s\n", datadir);
	printf("Username: postgres\n");
	printf("Encoding: UTF8\n");
	printf("Locale: C\n\n");

	/* Call embedded initdb */
	if (pg_embedded_initdb(datadir, "postgres", "UTF8", "C") != 0)
	{
		fprintf(stderr, "\nERROR: initdb failed: %s\n", pg_embedded_error_message());
		return 1;
	}

	printf("\n========================================\n");
	printf("Success!\n");
	printf("========================================\n\n");
	printf("Database cluster initialized in: %s\n\n", datadir);
	printf("You can now use this directory with test_embedded:\n");
	printf("  ./test_embedded %s\n\n", datadir);

	return 0;
}
