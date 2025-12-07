/*
 * initdb_embedded.h
 *   Embedded in-process database initialization
 *
 * This provides in-process initdb functionality without forking
 * external postgres processes.
 */

#ifndef INITDB_EMBEDDED_H
#define INITDB_EMBEDDED_H

/*
 * pg_embedded_initdb_main
 *
 * Initialize a PostgreSQL database directory entirely in-process.
 *
 * Parameters:
 *   data_dir  - Path to data directory (will be created if doesn't exist)
 *   username  - Superuser name
 *   encoding  - Database encoding (e.g., "UTF8", NULL for UTF8)
 *   locale    - Locale setting (e.g., "C", NULL for C)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int pg_embedded_initdb_main(const char *data_dir,
                             const char *username,
                             const char *encoding,
                             const char *locale);

/*
 * pg_embedded_init_with_system_mods
 *
 * Internal function: Initialize embedded PostgreSQL with system table
 * modifications enabled. This is needed during initdb to run the
 * post-bootstrap SQL scripts that modify system catalogs.
 *
 * Returns:
 *   0 on success, -1 on error
 */
int pg_embedded_init_with_system_mods(const char *data_dir,
									  const char *dbname,
									  const char *username);

#endif /* INITDB_EMBEDDED_H */
