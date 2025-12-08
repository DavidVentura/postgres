/*
 * test_embedded.c - Test application for embedded PostgreSQL
 *
 * This demonstrates using PostgreSQL as an embedded database via the
 * pgembedded API. All database operations run in-process with no network
 * or IPC overhead.
 *
 * Usage:
 *   ./test_embedded <data_directory>
 *
 * Example:
 *   ./test_embedded /tmp/pgdata
 */

#include <stdio.h>
#include <stdlib.h>

#include "src/backend/embedded/pgembedded.h"

static void
print_result(pg_result *result)
{
	uint64_t	row;
	int			col;

	if (!result)
	{
		printf("NULL result\n");
		return;
	}

	printf("Status: %d, Rows: %llu, Cols: %d\n",
		   result->status,
		   (unsigned long long) result->rows,
		   result->cols);

	if (result->cols > 0 && result->colnames)
	{
		printf("\nColumn names:\n");
		for (col = 0; col < result->cols; col++)
		{
			printf("  [%d] %s\n", col, result->colnames[col]);
		}
	}

	if (result->values)
	{
		printf("\nData:\n");
		for (row = 0; row < result->rows; row++)
		{
			printf("  Row %llu: ", (unsigned long long) row);
			for (col = 0; col < result->cols; col++)
			{
				if (col > 0)
					printf(", ");
				printf("%s", result->values[row][col]);
			}
			printf("\n");
		}
	}

	printf("\n");
}

int
main(int argc, char **argv)
{
	const char *datadir;
	pg_result  *result;

	printf("========================================\n");
	printf("PostgreSQL Embedded Test Application\n");
	printf("========================================\n\n");

	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <data_directory>\n", argv[0]);
		fprintf(stderr, "\nExample:\n");
		fprintf(stderr, "  %s /tmp/pgdata\n\n", argv[0]);
		fprintf(stderr, "Note: Data directory must be initialized with initdb first\n");
		return 1;
	}

	datadir = argv[1];

	printf("Initializing PostgreSQL...\n");
	printf("  Data directory: %s\n", datadir);
	printf("  Database: postgres\n");
	printf("  User: postgres\n\n");

	if (pg_embedded_init(datadir, "postgres", "postgres") != 0)
	{
		fprintf(stderr, "ERROR: Initialization failed: %s\n",
				pg_embedded_error_message());
		return 1;
	}

	printf("PostgreSQL initialized successfully!\n\n");

	/* Test 1: Get PostgreSQL version */
	printf("----------------------------------------\n");
	printf("Test 1: Get PostgreSQL version\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec("SELECT version()");
	if (result && result->status < 0)
	{
		fprintf(stderr, "ERROR: %s\n", pg_embedded_error_message());
	}
	print_result(result);
	pg_embedded_free_result(result);

	/* Check current database */
	printf("Current database: ");
	result = pg_embedded_exec("SELECT current_database()");
	if (result && result->rows > 0)
	{
		printf("%s\n\n", result->values[0][0]);
	}
	pg_embedded_free_result(result);

	/* Test 2: Create a test table */
	printf("----------------------------------------\n");
	printf("Test 2: Create test table\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec("DROP TABLE IF EXISTS test_embedded");
	pg_embedded_free_result(result);

	result = pg_embedded_exec(
							  "CREATE TABLE test_embedded ("
							  "  id SERIAL PRIMARY KEY,"
							  "  name TEXT NOT NULL,"
							  "  value INTEGER"
							  ")");
	printf("DEBUG: Error message is: '%s'\n", pg_embedded_error_message());
	if (result && result->status < 0)
	{
		fprintf(stderr, "ERROR: CREATE TABLE failed: %s\n", pg_embedded_error_message());
	}
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 3: Insert data */
	printf("----------------------------------------\n");
	printf("Test 3: Insert test data\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec(
							  "INSERT INTO test_embedded (name, value) VALUES "
							  "('Alice', 100), "
							  "('Bob', 200), "
							  "('Charlie', 300)");
	if (result && result->status < 0)
	{
		fprintf(stderr, "ERROR: %s\n", pg_embedded_error_message());
	}
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 4: Query data */
	printf("----------------------------------------\n");
	printf("Test 4: Query test data\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec(
							  "SELECT id, name, value "
							  "FROM test_embedded "
							  "ORDER BY id");
	if (result && result->status < 0)
	{
		fprintf(stderr, "ERROR: %s\n", pg_embedded_error_message());
	}
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 5: Aggregation */
	printf("----------------------------------------\n");
	printf("Test 5: Aggregate query\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec(
							  "SELECT COUNT(*) as count, SUM(value) as total "
							  "FROM test_embedded");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 6: Transaction */
	printf("----------------------------------------\n");
	printf("Test 6: Transaction test\n");
	printf("----------------------------------------\n");

	printf("BEGIN transaction...\n");
	if (pg_embedded_begin() != 0)
	{
		fprintf(stderr, "ERROR: BEGIN failed: %s\n",
				pg_embedded_error_message());
	}

	result = pg_embedded_exec(
							  "INSERT INTO test_embedded (name, value) VALUES ('David', 400)");
	print_result(result);
	pg_embedded_free_result(result);

	printf("ROLLBACK transaction...\n");
	if (pg_embedded_rollback() != 0)
	{
		fprintf(stderr, "ERROR: ROLLBACK failed: %s\n",
				pg_embedded_error_message());
	}

	printf("Verify rollback (David should not appear):\n");
	result = pg_embedded_exec("SELECT name FROM test_embedded ORDER BY id");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 7: Successful transaction */
	printf("----------------------------------------\n");
	printf("Test 7: Committed transaction\n");
	printf("----------------------------------------\n");

	printf("BEGIN transaction...\n");
	pg_embedded_begin();

	result = pg_embedded_exec(
							  "INSERT INTO test_embedded (name, value) VALUES ('Eve', 500)");
	print_result(result);
	pg_embedded_free_result(result);

	printf("COMMIT transaction...\n");
	if (pg_embedded_commit() != 0)
	{
		fprintf(stderr, "ERROR: COMMIT failed: %s\n",
				pg_embedded_error_message());
	}

	printf("Verify commit (Eve should appear):\n");
	result = pg_embedded_exec("SELECT name FROM test_embedded ORDER BY id");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 8: INSERT...RETURNING */
	printf("----------------------------------------\n");
	printf("Test 8: INSERT...RETURNING\n");
	printf("----------------------------------------\n");

	result = pg_embedded_exec(
							  "INSERT INTO test_embedded (name, value) VALUES "
							  "('Frank', 600), ('Grace', 700) "
							  "RETURNING id, name, value");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 9: UPDATE...RETURNING */
	printf("----------------------------------------\n");
	printf("Test 9: UPDATE...RETURNING\n");
	printf("----------------------------------------\n");

	result = pg_embedded_exec(
							  "UPDATE test_embedded "
							  "SET value = value + 50 "
							  "WHERE name IN ('Alice', 'Bob') "
							  "RETURNING id, name, value");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 10: DELETE...RETURNING */
	printf("----------------------------------------\n");
	printf("Test 10: DELETE...RETURNING\n");
	printf("----------------------------------------\n");

	result = pg_embedded_exec(
							  "DELETE FROM test_embedded "
							  "WHERE value > 500 "
							  "RETURNING id, name, value");
	print_result(result);
	pg_embedded_free_result(result);

	/* Verify remaining data */
	printf("Verify remaining data after DELETE:\n");
	result = pg_embedded_exec("SELECT id, name, value FROM test_embedded ORDER BY id");
	print_result(result);
	pg_embedded_free_result(result);

	/* Test 11: ALTER TABLE on non-existent table (should fail) */
	printf("----------------------------------------\n");
	printf("Test 11: ALTER TABLE on non-existent table\n");
	printf("----------------------------------------\n");
	printf("Attempting to ALTER a table that doesn't exist...\n");
	result = pg_embedded_exec("ALTER TABLE nonexistent_table ADD COLUMN new_col INTEGER");
	if (result)
	{
		if (result->status < 0)
		{
			printf("Expected error occurred: %s\n", pg_embedded_error_message());
		}
		else
		{
			printf("Unexpected success!\n");
		}
		print_result(result);
		pg_embedded_free_result(result);
	}
	else
	{
		printf("Result was NULL - Error: %s\n", pg_embedded_error_message());
	}

	/* Cleanup */
	printf("----------------------------------------\n");
	printf("Cleanup\n");
	printf("----------------------------------------\n");
	result = pg_embedded_exec("DROP TABLE test_embedded");
	pg_embedded_free_result(result);

	printf("\nShutting down PostgreSQL...\n");
	pg_embedded_shutdown();

	printf("\n========================================\n");
	printf("All tests completed successfully!\n");
	printf("========================================\n");

	return 0;
}
