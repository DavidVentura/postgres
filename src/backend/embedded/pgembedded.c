/*-------------------------------------------------------------------------
 *
 * pgembedded.c
 *	  PostgreSQL Embedded API implementation
 *
 * This file implements a simple embedded database interface that wraps
 * PostgreSQL's single-user mode and SPI (Server Programming Interface).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/backend/embedded/pgembedded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pgembedded.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "executor/spi.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/* Static state */
static bool pg_initialized = false;
static char pg_error_msg[1024] = {0};

/*
 * pg_embedded_initdb
 *
 * Initialize a new PostgreSQL data directory by calling BootstrapModeMain
 * directly. This creates the system catalogs and template databases.
 *
 * NOTE: This is a complex operation that normally requires the postgres.bki
 * file and other share files. For simplicity, this implementation calls
 * the system initdb command if available, or provides instructions.
 */
int
pg_embedded_initdb(const char *data_dir, const char *username,
				   const char *encoding, const char *locale)
{
	char		cmd[2048];
	int			ret;
	const char *initdb_paths[] = {
		"/usr/lib/postgresql/17/bin/initdb",
		"/usr/lib/postgresql/16/bin/initdb",
		"/usr/lib/postgresql/15/bin/initdb",
		"/usr/local/bin/initdb",
		"/usr/bin/initdb",
		"initdb",					/* Try PATH */
		NULL
	};
	int			i;
	bool		found = false;

	if (!data_dir || !username)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "data_dir and username are required");
		return -1;
	}

	/*
	 * Try to find a compatible initdb. We look in common PostgreSQL
	 * installation paths.
	 */
	for (i = 0; initdb_paths[i] != NULL; i++)
	{
		snprintf(cmd, sizeof(cmd),
				 "%s --version > /dev/null 2>&1",
				 initdb_paths[i]);

		if (system(cmd) == 0)
		{
			found = true;
			break;
		}
	}

	if (!found)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "Could not find initdb command. Please install PostgreSQL or "
				 "create the data directory manually with: "
				 "initdb -D %s -U %s -A trust",
				 data_dir, username);
		return -1;
	}

	/* Build and run initdb command */
	snprintf(cmd, sizeof(cmd),
			 "%s -D \"%s\" -U \"%s\" %s%s %s%s -A trust 2>&1",
			 initdb_paths[i],
			 data_dir,
			 username,
			 encoding ? "-E " : "",
			 encoding ? encoding : "",
			 locale ? "--locale=" : "",
			 locale ? locale : "");

	fprintf(stderr, "Initializing database: %s\n", cmd);

	ret = system(cmd);

	if (ret != 0)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "initdb command failed with exit code %d",
				 ret);
		return -1;
	}

	fprintf(stderr, "Database initialized successfully\n");
	return 0;
}

/*
 * pg_embedded_init
 *
 * Initialize PostgreSQL in single-user embedded mode
 */
int
pg_embedded_init(const char *data_dir, const char *dbname, const char *username)
{
	if (pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Already initialized");
		return 0;				/* Already initialized, not an error */
	}

	if (!data_dir || !dbname || !username)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Invalid arguments");
		return -1;
	}

	PG_TRY();
	{
		/*
		 * Essential early initialization (from main.c)
		 * Must happen before anything else, including InitStandaloneProcess
		 */
		MyProcPid = getpid();
		MyStartTime = time(NULL);

		/* Initialize memory context system - CRITICAL! */
		MemoryContextInit();

		/*
		 * Set up the executable path. For embedded use, we use argv[0] which
		 * should be set to progname. If my_exec_path hasn't been set yet,
		 * find it now.
		 */
		if (my_exec_path[0] == '\0')
		{
			if (find_my_exec(progname, my_exec_path) < 0)
			{
				/* If we can't find it, just use progname as a fallback */
				strlcpy(my_exec_path, progname, MAXPGPATH);
			}
		}

		if (pkglib_path[0] == '\0')
			get_pkglib_path(my_exec_path, pkglib_path);

		/* Set data directory */
		SetDataDir(data_dir);

		/* Initialize as standalone backend */
		InitStandaloneProcess(progname);

		/* Initialize configuration */
		InitializeGUCOptions();

		/* Load configuration files */
		SelectConfigFiles(NULL, username);

		/* Validate and switch to data directory */
		checkDataDir();
		ChangeToDataDir();

		/* Create lockfile */
		CreateDataDirLockFile(false);

		/* Read control file */
		LocalProcessControlFile(false);

		/* Load shared libraries */
		process_shared_preload_libraries();

		/* Initialize MaxBackends */
		InitializeMaxBackends();

		/*
		 * We don't need postmaster child slots in single-user mode, but
		 * initialize them anyway to avoid having special handling.
		 */
		InitPostmasterChildSlots();

		/* Initialize size of fast-path lock cache. */
		InitializeFastPathLocks();

		/*
		 * Give preloaded libraries a chance to request additional shared memory.
		 */
		process_shmem_requests();

		/*
		 * Now that loadable modules have had their chance to request additional
		 * shared memory, determine the value of any runtime-computed GUCs that
		 * depend on the amount of shared memory required.
		 */
		InitializeShmemGUCs();

		/*
		 * Now that modules have been loaded, we can process any custom resource
		 * managers specified in the wal_consistency_checking GUC.
		 */
		InitializeWalConsistencyChecking();

		/*
		 * Create shared memory etc.  (Nothing's really "shared" in single-user
		 * mode, but we must have these data structures anyway.)
		 */
		CreateSharedMemoryAndSemaphores();

		/*
		 * Estimate number of openable files.  This must happen after setting up
		 * semaphores, because on some platforms semaphores count as open files.
		 */
		set_max_safe_fds();

		/*
		 * Remember stand-alone backend startup time,roughly at the same point
		 * during startup that postmaster does so.
		 */
		PgStartTime = GetCurrentTimestamp();

		/*
		 * Create a per-backend PGPROC struct in shared memory. We must do this
		 * before we can use LWLocks.
		 */
		InitProcess();

		/* Early backend initialization */
		BaseInit();

		/* Connect to specified database */
		InitPostgres(dbname, InvalidOid, username, InvalidOid, 0, NULL);

		/*
		 * If the PostmasterContext is still around, recycle the space; we don't
		 * need it anymore after InitPostgres completes.
		 */
		if (PostmasterContext)
		{
			MemoryContextDelete(PostmasterContext);
			PostmasterContext = NULL;
		}

		/* Set processing mode to normal */
		SetProcessingMode(NormalProcessing);

		/* Disable output to stdout/stderr */
		whereToSendOutput = DestNone;

		/*
		 * Create the memory context for query processing.
		 * MessageContext is used for query execution and is reset after each query.
		 */
		MessageContext = AllocSetContextCreate(TopMemoryContext,
											   "MessageContext",
											   ALLOCSET_DEFAULT_SIZES);

		/* Connect to SPI (Server Programming Interface) */
		if (SPI_connect() != SPI_OK_CONNECT)
		{
			snprintf(pg_error_msg, sizeof(pg_error_msg), "SPI_connect failed");
			return -1;
		}

		pg_initialized = true;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Get error data */
		edata = CopyErrorData();
		FlushErrorState();

		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "Initialization failed: %s", edata->message);

		FreeErrorData(edata);
		return -1;
	}
	PG_END_TRY();

	return 0;
}

/*
 * pg_embedded_exec
 *
 * Execute SQL query and return results
 */
pg_result *
pg_embedded_exec(const char *query)
{
	pg_result  *result;
	int			ret;

	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return NULL;
	}

	if (!query)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "NULL query");
		return NULL;
	}

	/* Allocate result structure */
	result = (pg_result *) malloc(sizeof(pg_result));
	if (!result)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Out of memory");
		return NULL;
	}

	memset(result, 0, sizeof(pg_result));

	PG_TRY();
	{
		/*
		 * Start a transaction if not already in one.
		 * This sets up CurrentResourceOwner which SPI needs.
		 */
		if (!IsTransactionState())
			StartTransactionCommand();

		/*
		 * Push an active snapshot for query execution.
		 * SPI requires a snapshot to be active.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Execute query via SPI */
		ret = SPI_execute(query, false, 0);		/* false = read-write, 0 = no
												 * row limit */

		result->status = ret;
		result->rows = SPI_processed;
		result->cols = 0;
		result->values = NULL;
		result->colnames = NULL;

		if (ret < 0)
		{
			snprintf(pg_error_msg, sizeof(pg_error_msg),
					 "Query execution failed with code: %d", ret);
			return result;
		}

		/* For SELECT queries, copy result data */
		if (ret == SPI_OK_SELECT && SPI_tuptable != NULL)
		{
			SPITupleTable *tuptable = SPI_tuptable;
			TupleDesc	tupdesc = tuptable->tupdesc;
			uint64_t	row;
			int			col;

			result->cols = tupdesc->natts;

			/* Allocate column names array */
			result->colnames = (char **) malloc(result->cols * sizeof(char *));
			if (!result->colnames)
			{
				snprintf(pg_error_msg, sizeof(pg_error_msg), "Out of memory");
				pg_embedded_free_result(result);
				return NULL;
			}

			/* Copy column names */
			for (col = 0; col < result->cols; col++)
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, col);

				result->colnames[col] = strdup(NameStr(attr->attname));
			}

			/* Allocate result matrix */
			result->values = (char ***) malloc(result->rows * sizeof(char **));
			if (!result->values)
			{
				snprintf(pg_error_msg, sizeof(pg_error_msg), "Out of memory");
				pg_embedded_free_result(result);
				return NULL;
			}

			/* Copy data for each row */
			for (row = 0; row < result->rows; row++)
			{
				HeapTuple	tuple = tuptable->vals[row];

				result->values[row] = (char **) malloc(result->cols * sizeof(char *));
				if (!result->values[row])
				{
					snprintf(pg_error_msg, sizeof(pg_error_msg), "Out of memory");
					pg_embedded_free_result(result);
					return NULL;
				}

				/* Get each column value */
				for (col = 0; col < result->cols; col++)
				{
					bool		isnull;
					char	   *str;

					str = SPI_getvalue(tuple, tupdesc, col + 1);

					if (str == NULL)
						result->values[row][col] = strdup("NULL");
					else
					{
						result->values[row][col] = strdup(str);
						pfree(str);
					}
				}
			}
		}

		/*
		 * Pop the active snapshot.
		 */
		PopActiveSnapshot();

		/*
		 * Note: We don't commit the transaction here. Transactions are managed
		 * explicitly by the caller using BEGIN/COMMIT/ROLLBACK, or implicitly
		 * by the SPI layer. Committing here would interfere with multi-statement
		 * transactions.
		 */
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Get error data */
		edata = CopyErrorData();
		FlushErrorState();

		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "Query failed: %s", edata->message);

		FreeErrorData(edata);

		/*
		 * Note: We don't abort the transaction here either. Let the caller
		 * decide whether to commit or rollback. PostgreSQL's error handling
		 * will mark the transaction as aborted if needed.
		 */

		/* Clean up and return partial result with error */
		result->status = -1;
		return result;
	}
	PG_END_TRY();

	return result;
}

/*
 * pg_embedded_free_result
 *
 * Free result structure
 */
void
pg_embedded_free_result(pg_result *result)
{
	uint64_t	row;
	int			col;

	if (!result)
		return;

	/* Free column names */
	if (result->colnames)
	{
		for (col = 0; col < result->cols; col++)
		{
			if (result->colnames[col])
				free(result->colnames[col]);
		}
		free(result->colnames);
	}

	/* Free data values */
	if (result->values)
	{
		for (row = 0; row < result->rows; row++)
		{
			if (result->values[row])
			{
				for (col = 0; col < result->cols; col++)
				{
					if (result->values[row][col])
						free(result->values[row][col]);
				}
				free(result->values[row]);
			}
		}
		free(result->values);
	}

	free(result);
}

/*
 * pg_embedded_begin
 *
 * Begin a transaction
 */
int
pg_embedded_begin(void)
{
	pg_result  *result;

	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return -1;
	}

	result = pg_embedded_exec("BEGIN");
	if (result == NULL || result->status < 0)
	{
		if (result)
			pg_embedded_free_result(result);
		return -1;
	}

	pg_embedded_free_result(result);
	return 0;
}

/*
 * pg_embedded_commit
 *
 * Commit current transaction
 */
int
pg_embedded_commit(void)
{
	pg_result  *result;

	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return -1;
	}

	result = pg_embedded_exec("COMMIT");
	if (result == NULL || result->status < 0)
	{
		if (result)
			pg_embedded_free_result(result);
		return -1;
	}

	pg_embedded_free_result(result);
	return 0;
}

/*
 * pg_embedded_rollback
 *
 * Rollback current transaction
 */
int
pg_embedded_rollback(void)
{
	pg_result  *result;

	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return -1;
	}

	result = pg_embedded_exec("ROLLBACK");
	if (result == NULL || result->status < 0)
	{
		if (result)
			pg_embedded_free_result(result);
		return -1;
	}

	pg_embedded_free_result(result);
	return 0;
}

/*
 * pg_embedded_error_message
 *
 * Get last error message
 */
const char *
pg_embedded_error_message(void)
{
	return pg_error_msg;
}

/*
 * pg_embedded_shutdown
 *
 * Shutdown embedded PostgreSQL instance
 */
void
pg_embedded_shutdown(void)
{
	if (!pg_initialized)
		return;

	PG_TRY();
	{
		/* Disconnect from SPI */
		SPI_finish();

		/* Perform normal backend exit */
		proc_exit(0);
	}
	PG_CATCH();
	{
		/* Ignore errors during shutdown */
		FlushErrorState();
	}
	PG_END_TRY();

	pg_initialized = false;
}
