/*
 * test_initdb.c - Test in-process database initialization
 *
 * This tests the pg_embedded_initdb() function which creates
 * a new PostgreSQL database cluster in-process.
 */

#include <stdio.h>
#include <stdlib.h>

#include "src/backend/embedded/pgembedded.h"

int
main(int argc, char **argv)
{
	const char *datadir;

	printf("========================================\n");
	printf("Test: In-Process Database Initialization\n");
	printf("========================================\n\n");

	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <data_directory>\n", argv[0]);
		fprintf(stderr, "\nExample:\n");
		fprintf(stderr, "  %s /tmp/pgdata_new\n\n", argv[0]);
		fprintf(stderr, "This will create a new PostgreSQL database cluster\n");
		fprintf(stderr, "in the specified directory.\n\n");
		return 1;
	}

	datadir = argv[1];

	printf("Creating new database cluster...\n");
	printf("  Data directory: %s\n", datadir);
	printf("  User: postgres\n");
	printf("  Encoding: UTF8\n");
	printf("  Locale: C\n\n");

	/* Call pg_embedded_initdb to create the database cluster */
	if (pg_embedded_initdb(datadir, "postgres", "UTF8", "C") != 0)
	{
		fprintf(stderr, "\nERROR: Database initialization failed: %s\n",
				pg_embedded_error_message());
		return 1;
	}

	printf("\n========================================\n");
	printf("Database cluster created successfully!\n");
	printf("========================================\n\n");

	printf("Next steps:\n");
	printf("  1. Use test_embedded to connect and run queries:\n");
	printf("     ./test_embedded %s\n\n", datadir);

	return 0;
}
