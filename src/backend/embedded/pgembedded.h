/*-------------------------------------------------------------------------
 *
 * pgembedded.h
 *	  PostgreSQL Embedded API - Clean interface for in-process database
 *
 * This API provides a simple way to use PostgreSQL as an embedded database
 * running in single-user mode within the same process. No network sockets
 * or IPC are used - all operations are in-process.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/backend/embedded/pgembedded.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EMBEDDED_H
#define PG_EMBEDDED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Query result structure
 */
typedef struct pg_result
{
	int			status;			/* SPI_OK_SELECT, SPI_OK_INSERT, etc. */
	uint64_t	rows;			/* Number of rows affected/returned */
	int			cols;			/* Number of columns (for SELECT) */
	char	 ***values;			/* Result data [row][col] as strings */
	char	  **colnames;		/* Column names */
} pg_result;

/*
 * Initialization and shutdown
 */

/* Initialize a new PostgreSQL data directory (like initdb)
 *
 * data_dir: Path where to create the data directory
 * username: Superuser name (e.g., "postgres")
 * encoding: Database encoding (e.g., "UTF8", or NULL for default)
 * locale: Locale (e.g., "C", or NULL for default)
 *
 * Returns 0 on success, -1 on failure
 * NOTE: This must be called BEFORE pg_embedded_init if creating a new database
 */
int pg_embedded_initdb(const char *data_dir, const char *username,
                       const char *encoding, const char *locale);

/* Initialize embedded PostgreSQL instance
 *
 * data_dir: Path to initialized PostgreSQL data directory
 * dbname: Database name to connect to (e.g., "postgres")
 * username: Username for the session (e.g., "postgres")
 *
 * Returns 0 on success, -1 on failure
 */
int pg_embedded_init(const char *data_dir, const char *dbname, const char *username);

/* Shutdown embedded PostgreSQL instance */
void pg_embedded_shutdown(void);

/*
 * Query execution
 */

/* Execute SQL query and return results
 *
 * query: SQL query string
 *
 * Returns result structure (must be freed with pg_embedded_free_result)
 * Returns NULL on error (check pg_embedded_error_message)
 */
pg_result *pg_embedded_exec(const char *query);

/* Free result structure returned by pg_embedded_exec */
void pg_embedded_free_result(pg_result *result);

/*
 * Transaction control
 */

/* Begin a transaction - returns 0 on success, -1 on error */
int pg_embedded_begin(void);

/* Commit current transaction - returns 0 on success, -1 on error */
int pg_embedded_commit(void);

/* Rollback current transaction - returns 0 on success, -1 on error */
int pg_embedded_rollback(void);

/*
 * Error handling
 */

/* Get last error message - returns pointer to static string */
const char *pg_embedded_error_message(void);

#ifdef __cplusplus
}
#endif

#endif							/* PG_EMBEDDED_H */
