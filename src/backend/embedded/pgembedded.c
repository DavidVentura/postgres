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
#include "initdb_embedded.h"

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
 * Initialize a new PostgreSQL data directory in-process.
 * This creates the system catalogs and template databases without
 * forking external processes.
 */
int
pg_embedded_initdb(const char *data_dir, const char *username,
				   const char *encoding, const char *locale)
{
	int ret;

	if (!data_dir || !username)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "data_dir and username are required");
		return -1;
	}

	/* Call the in-process initdb implementation */
	ret = pg_embedded_initdb_main(data_dir, username, encoding, locale);

	if (ret != 0)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "initdb failed");
		return -1;
	}

	return 0;
}

/*
 * pg_embedded_init_internal
 *
 * Initialize PostgreSQL in single-user embedded mode
 * If allow_system_table_mods is true, enables modification of system catalogs
 */
static int
pg_embedded_init_internal(const char *data_dir, const char *dbname,
						  const char *username, bool allow_system_table_mods)
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

		/*
		 * Enable system table modifications if requested (needed for initdb).
		 * This must be set before SelectConfigFiles() is called.
		 */
		if (allow_system_table_mods)
			SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);

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

		/*
		 * Perform an empty transaction to finalize SPI setup.
		 * This ensures the system is ready for query execution.
		 */
		StartTransactionCommand();
		if (SPI_connect() != SPI_OK_CONNECT)
		{
			snprintf(pg_error_msg, sizeof(pg_error_msg), "SPI_connect failed");
			AbortCurrentTransaction();
			return -1;
		}
		SPI_finish();
		CommitTransactionCommand();

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
 * pg_embedded_init
 *
 * Public wrapper for normal use (no system table mods)
 */
int
pg_embedded_init(const char *data_dir, const char *dbname, const char *username)
{
	return pg_embedded_init_internal(data_dir, dbname, username, false);
}

/*
 * pg_embedded_init_with_system_mods
 *
 * Initialize with system table modifications enabled (for initdb)
 */
int
pg_embedded_init_with_system_mods(const char *data_dir, const char *dbname, const char *username)
{
	return pg_embedded_init_internal(data_dir, dbname, username, true);
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
		bool		implicit_tx = false;

		/*
		 * Transaction Handling Strategy:
		 * If we are NOT in a transaction, we act as "Auto-commit":
		 * Start -> Exec -> Commit.
		 * If we ARE in a transaction (via pg_embedded_begin), we just Exec.
		 */
		if (!IsTransactionState())
		{
			StartTransactionCommand();
			implicit_tx = true;
		}

		/*
		 * SPI requires a snapshot to be active.
		 * Push an active snapshot for query execution.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Connect to SPI for this query */
		if (SPI_connect() != SPI_OK_CONNECT)
		{
			snprintf(pg_error_msg, sizeof(pg_error_msg), "SPI_connect failed");
			PopActiveSnapshot();
			if (implicit_tx)
				AbortCurrentTransaction();
			result->status = -1;
			return result;
		}

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
			PopActiveSnapshot();
			if (implicit_tx)
				AbortCurrentTransaction();
			return result;
		}

		/*
		 * Copy data for queries with results (SELECT or RETURNING)
		 */
		if (ret > 0 && SPI_tuptable != NULL)
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
					char	   *str;

					str = SPI_getvalue(tuple, tupdesc, col + 1);

					if (str == NULL)
						result->values[row][col] = NULL;
					else
					{
						result->values[row][col] = strdup(str);
						pfree(str);
					}
				}
			}
		}

		/* Disconnect from SPI */
		SPI_finish();

		/*
		 * Pop the active snapshot.
		 */
		PopActiveSnapshot();

		/*
		 * If we started the transaction implicitly (auto-commit mode),
		 * commit it now. Otherwise, the transaction was started explicitly
		 * via pg_embedded_begin() and the caller will manage it.
		 */
		if (implicit_tx)
			CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Get error data and copy message before aborting */
		edata = CopyErrorData();
		FlushErrorState();

		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "Query failed: %s", edata->message);

		/*
		 * Abort the current transaction BEFORE freeing error data.
		 * This ensures we don't try to free memory from destroyed contexts.
		 */
		AbortCurrentTransaction();

		/* Now it's safe to free - but DON'T call FreeErrorData after abort */
		/* The memory will be cleaned up by the memory context reset */

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
	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return -1;
	}

	if (IsTransactionState())
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Already in transaction");
		return -1;
	}

	PG_TRY();
	{
		StartTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		edata = CopyErrorData();
		FlushErrorState();
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "BEGIN failed: %s", edata->message);
		FreeErrorData(edata);
		AbortCurrentTransaction();
		return -1;
	}
	PG_END_TRY();

	return 0;
}

/*
 * pg_embedded_commit
 *
 * Commit current transaction using the C API
 */
int
pg_embedded_commit(void)
{
	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not initialized");
		return -1;
	}

	if (!IsTransactionState())
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not in transaction");
		return -1;
	}

	PG_TRY();
	{
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		edata = CopyErrorData();
		FlushErrorState();
		snprintf(pg_error_msg, sizeof(pg_error_msg),
				 "COMMIT failed: %s", edata->message);
		FreeErrorData(edata);
		AbortCurrentTransaction();
		return -1;
	}
	PG_END_TRY();

	return 0;
}

/*
 * pg_embedded_rollback
 *
 * Rollback current transaction using the C API
 */
int
pg_embedded_rollback(void)
{
	if (!pg_initialized)
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not in transaction");
		return -1;
	}

	if (!IsTransactionState())
	{
		snprintf(pg_error_msg, sizeof(pg_error_msg), "Not in transaction");
		return -1;
	}

	PG_TRY();
	{
		AbortCurrentTransaction();
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();

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
		/*
		 * Use shmem_exit(0) instead of proc_exit(0).
		 * This runs all the internal PostgreSQL cleanup hooks
		 * (closing WAL, flushing buffers, releasing locks) but does NOT
		 * call exit() and kill the host application process.
		 */
		shmem_exit(0);
	}
	PG_CATCH();
	{
		/* Ignore errors during shutdown */
		FlushErrorState();
	}
	PG_END_TRY();

	pg_initialized = false;
}
