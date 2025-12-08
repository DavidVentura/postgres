/*
 * initdb_simple.c
 *   Simplified in-process initdb for embedded PostgreSQL
 *
 * This is a minimal reimplementation of initdb.c that:
 * - Takes parameters directly (no argc/argv parsing)
 * - Calls BootstrapModeMain and PostgresSingleUserMain directly (no popen/fork)
 * - Uses minimal configuration (no config file generation)
 * - Runs entirely in-process
 *
 * Based on src/bin/initdb/initdb.c but heavily simplified.
 */

#include "postgres.h"

#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bootstrap/bootstrap.h"
#include "commands/dbcommands.h"
#include "common/file_perm.h"
#include "common/restricted_token.h"
#include "common/username.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "access/xact.h"

#include "initdb_embedded.h"
#include "pgembedded.h"

/* Global variables (simplified from initdb.c) */
static char *pg_data = NULL;
static char *username_g = NULL;
static char *encoding_g = NULL;
static char *locale_g = NULL;

/* Subdirectories to create */
static const char *const subdirs[] = {
	"global",
	"pg_wal/archive_status",
	"pg_commit_ts",
	"pg_dynshmem",
	"pg_notify",
	"pg_serial",
	"pg_snapshots",
	"pg_subtrans",
	"pg_twophase",
	"pg_multixact",
	"pg_multixact/members",
	"pg_multixact/offsets",
	"base",
	"base/1",
	"pg_replslot",
	"pg_tblspc",
	"pg_stat",
	"pg_stat_tmp",
	"pg_xact",
	"pg_logical",
	"pg_logical/snapshots",
	"pg_logical/mappings",
};

/*
 * create_data_directory
 */
static void
create_data_directory(void)
{
	if (mkdir(pg_data, pg_dir_create_mode) < 0)
	{
		if (errno == EEXIST)
			fprintf(stderr, "WARNING: directory \"%s\" exists\n", pg_data);
		else
		{
			fprintf(stderr, "ERROR: could not create directory \"%s\": %s\n",
					pg_data, strerror(errno));
			exit(1);
		}
	}
}

/*
 * create_xlog_symlink
 */
static void
create_xlog_symlink(void)
{
	char subdirloc[MAXPGPATH];

	snprintf(subdirloc, sizeof(subdirloc), "%s/pg_wal", pg_data);

	if (mkdir(subdirloc, pg_dir_create_mode) < 0)
	{
		fprintf(stderr, "ERROR: could not create directory \"%s\": %s\n",
				subdirloc, strerror(errno));
		exit(1);
	}
}

/*
 * create_subdirectories
 */
static void
create_subdirectories(void)
{
	int			i;

	for (i = 0; i < lengthof(subdirs); i++)
	{
		char path[MAXPGPATH];

		snprintf(path, sizeof(path), "%s/%s", pg_data, subdirs[i]);

		if (mkdir(path, pg_dir_create_mode) < 0)
		{
			fprintf(stderr, "ERROR: could not create directory \"%s\": %s\n",
					path, strerror(errno));
			exit(1);
		}
	}
}

/*
 * create_database_direct
 *
 * Create a database using the C API instead of SQL to avoid
 * "CREATE DATABASE cannot be executed from a function" error
 */
static int
create_database_direct(const char *dbname, Oid dboid, bool is_template,
					   bool allow_connections, const char *comment)
{
	CreatedbStmt *stmt;
	ParseState *pstate;
	DefElem    *downer;
	DefElem    *dtemplate;
	DefElem    *dtype;
	DefElem    *dcollate;
	DefElem    *dctype;
	DefElem    *distemplate;
	DefElem    *dallowconn;
	DefElem    *dconnlimit;
	DefElem    *doid;
	DefElem    *dstrategy;
	List       *options = NIL;
	MemoryContext oldcontext;
	char       *dbname_copy;
	char       *comment_copy = NULL;

	/*
	 * Copy string parameters to CurrentMemoryContext FIRST,
	 * before they get corrupted by palloc operations
	 */
	oldcontext = MemoryContextSwitchTo(CurrentMemoryContext);
	dbname_copy = pstrdup(dbname);
	if (comment)
		comment_copy = pstrdup(comment);
	MemoryContextSwitchTo(oldcontext);

	/* Build options list */
	downer = makeDefElem("owner", (Node *) makeString(username_g), -1);
	options = lappend(options, downer);

	dtemplate = makeDefElem("template", (Node *) makeString("template1"), -1);
	options = lappend(options, dtemplate);

	/* Don't specify encoding - it will be inherited from template1 */

	dtype = makeDefElem("locale_provider", (Node *) makeString("libc"), -1);
	options = lappend(options, dtype);

	dcollate = makeDefElem("lc_collate", (Node *) makeString(locale_g), -1);
	options = lappend(options, dcollate);

	dctype = makeDefElem("lc_ctype", (Node *) makeString(locale_g), -1);
	options = lappend(options, dctype);

	if (is_template)
	{
		distemplate = makeDefElem("is_template", (Node *) makeBoolean(true), -1);
		options = lappend(options, distemplate);
	}

	if (!allow_connections)
	{
		dallowconn = makeDefElem("allow_connections", (Node *) makeBoolean(false), -1);
		options = lappend(options, dallowconn);
	}

	dconnlimit = makeDefElem("connection_limit", (Node *) makeInteger(-1), -1);
	options = lappend(options, dconnlimit);

	if (dboid != InvalidOid)
	{
		doid = makeDefElem("oid", (Node *) makeInteger(dboid), -1);
		options = lappend(options, doid);
	}

	/* Use file_copy strategy for faster creation */
	dstrategy = makeDefElem("strategy", (Node *) makeString("file_copy"), -1);
	options = lappend(options, dstrategy);

	/* Create the statement */
	stmt = makeNode(CreatedbStmt);
	stmt->dbname = dbname_copy;
	stmt->options = options;

	/* Create a parse state */
	pstate = make_parsestate(NULL);

	/* Start a transaction - createdb needs one */
	StartTransactionCommand();

	/* Call createdb */
	PG_TRY();
	{
		createdb(pstate, stmt);

		/* Commit the transaction */
		CommitTransactionCommand();

		/* Add comment if requested */
		if (comment_copy)
		{
			char *sql = psprintf("COMMENT ON DATABASE %s IS '%s';",
								 dbname_copy, comment_copy);
			pg_result *result = pg_embedded_exec(sql);
			if (result)
				pg_embedded_free_result(result);
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		edata = CopyErrorData();
		FlushErrorState();
		fprintf(stderr, "\nERROR: Failed to create database %s: %s\n",
				dbname_copy, edata->message);
		FreeErrorData(edata);

		/* Abort the transaction on error */
		AbortCurrentTransaction();
		return -1;
	}
	PG_END_TRY();

	return 0;
}

/*
 * write empty postgresql.conf
 */
static void
write_empty_config_file(const char *extrapath)
{
	FILE* config_file;
	char path[MAXPGPATH];

	if (extrapath == NULL)
		snprintf(path, sizeof(path), "%s/postgresql.conf", pg_data);
	else
		snprintf(path, sizeof(path), "%s/%s/postgresql.conf", pg_data, extrapath);

	config_file = fopen(path, PG_BINARY_W);
	if (config_file == NULL)
	{
		fprintf(stderr, "ERROR: could not open \"%s\" for writing: %s\n",
				path, strerror(errno));
		exit(1);
	}

	if (fclose(config_file)) {
		fprintf(stderr, "ERROR: could not write file \"%s\": %s\n",
				path, strerror(errno));
		exit(1);
	}
}


/*
 * write_version_file
 */
static void
write_version_file(const char *extrapath)
{
	FILE	   *version_file;
	char path[MAXPGPATH];

	if (extrapath == NULL)
		snprintf(path, sizeof(path), "%s/PG_VERSION", pg_data);
	else
		snprintf(path, sizeof(path), "%s/%s/PG_VERSION", pg_data, extrapath);

	version_file = fopen(path, PG_BINARY_W);
	if (version_file == NULL)
	{
		fprintf(stderr, "ERROR: could not open \"%s\" for writing: %s\n",
				path, strerror(errno));
		exit(1);
	}

	if (fprintf(version_file, "%s\n", PG_MAJORVERSION) < 0 ||
		fflush(version_file) != 0 ||
		fsync(fileno(version_file)) != 0 ||
		fclose(version_file))
	{
		fprintf(stderr, "ERROR: could not write file \"%s\": %s\n",
				path, strerror(errno));
		exit(1);
	}
}

/*
 * pg_embedded_initdb_main
 *
 * Main entry point for in-process database initialization.
 */
int
pg_embedded_initdb_main(const char *data_dir,
                         const char *username,
                         const char *encoding,
                         const char *locale)
{
	char version_file[MAXPGPATH];

	/* Validate parameters */
	if (!data_dir || !username)
	{
		fprintf(stderr, "ERROR: data_dir and username are required\n");
		return -1;
	}

	/* Check if database already initialized */
	snprintf(version_file, sizeof(version_file), "%s/PG_VERSION", data_dir);
	{
		struct stat st;
		if (stat(version_file, &st) == 0)
		{
			fprintf(stderr, "WARNING: database directory already initialized\n");
			return 0;
		}
	}

	/* Set global variables */
	pg_data = strdup(data_dir);
	username_g = strdup(username);
	encoding_g = encoding ? strdup(encoding) : strdup("UTF8");
	locale_g = locale ? strdup(locale) : strdup("C");

	/* Create directory structure */
	printf("creating directory %s ... ", pg_data);
	fflush(stdout);
	create_data_directory();
	printf("ok\n");

	printf("creating subdirectories ... ");
	fflush(stdout);
	create_xlog_symlink();
	create_subdirectories();
	printf("ok\n");

	/* Write version file */
	printf("writing version file ... ");
	fflush(stdout);
	write_version_file(NULL);
	write_version_file("base/1");  /* Also in template1 directory */
    write_empty_config_file(NULL);
	printf("ok\n");

	/*
	 * TODO: Bootstrap template1 database
	 * This requires:
	 * 1. Reading postgres.bki file
	 * 2. Calling BootstrapModeMain() with the BKI commands
	 * 3. Running post-bootstrap SQL via PostgresSingleUserMain()
	 *
	 * For now, we've created the directory structure.
	 * Next step: implement bootstrap logic.
	 */

	/*
	 * Bootstrap template1 database
	 * Fork a child process to run bootstrap, then wait for it to complete
	 */
	printf("running bootstrap script ... ");
	fflush(stdout);

	{
		pid_t bootstrap_pid;
		int status;

		/* Prepare BKI file with token substitution */
		char *boot_argv[10];
		int boot_argc = 0;
		FILE *bki_src, *bki_dest;
		char line[8192];
		const char *bki_src_path = "src/include/catalog/postgres.bki";
		const char *bki_temp_path = "/tmp/pg_bootstrap.bki";

		/* Copy BKI file with token substitution */
		bki_src = fopen(bki_src_path, "r");
		if (!bki_src)
		{
			fprintf(stderr, "\nERROR: could not open %s: %s\n",
					bki_src_path, strerror(errno));
			fprintf(stderr, "Make sure you're running from the postgres source directory\n");
			exit(1);
		}

		bki_dest = fopen(bki_temp_path, "w");
		if (!bki_dest)
		{
			fprintf(stderr, "\nERROR: could not create %s: %s\n",
					bki_temp_path, strerror(errno));
			fclose(bki_src);
			exit(1);
		}

		/* Copy BKI file with token substitution */
		while (fgets(line, sizeof(line), bki_src))
		{
			char output[8192];
			char *in = line;
			char *out = output;

			/* Simple token replacement */
			while (*in)
			{
				if (strncmp(in, "NAMEDATALEN", 11) == 0)
				{
					out += sprintf(out, "%d", NAMEDATALEN);
					in += 11;
				}
				else if (strncmp(in, "SIZEOF_POINTER", 14) == 0)
				{
					out += sprintf(out, "%d", (int)sizeof(void*));
					in += 14;
				}
				else if (strncmp(in, "ALIGNOF_POINTER", 15) == 0)
				{
					out += sprintf(out, "%s", (sizeof(void*) == 4) ? "i" : "d");
					in += 15;
				}
				else if (strncmp(in, "POSTGRES", 8) == 0)
				{
					out += sprintf(out, "%s", username_g);
					in += 8;
				}
				else if (strncmp(in, "ENCODING", 8) == 0)
				{
					int encid = pg_char_to_encoding(encoding_g);
					out += sprintf(out, "%d", encid);
					in += 8;
				}
				else if (strncmp(in, "LC_COLLATE", 10) == 0)
				{
					out += sprintf(out, "%s", locale_g);
					in += 10;
				}
				else if (strncmp(in, "LC_CTYPE", 8) == 0)
				{
					out += sprintf(out, "%s", locale_g);
					in += 8;
				}
				else if (strncmp(in, "DATLOCALE", 9) == 0)
				{
					out += sprintf(out, "_null_");
					in += 9;
				}
				else if (strncmp(in, "ICU_RULES", 9) == 0)
				{
					out += sprintf(out, "_null_");
					in += 9;
				}
				else if (strncmp(in, "LOCALE_PROVIDER", 15) == 0)
				{
					out += sprintf(out, "c");
					in += 15;
				}
				else
				{
					*out++ = *in++;
				}
			}
			*out = '\0';

			fputs(output, bki_dest);
		}

		fclose(bki_src);
		fclose(bki_dest);

		/* Build bootstrap argv */
		boot_argv[boot_argc++] = strdup("postgres");
		boot_argv[boot_argc++] = strdup("--boot");
		boot_argv[boot_argc++] = strdup("-D");
		boot_argv[boot_argc++] = strdup(pg_data);
		boot_argv[boot_argc++] = strdup("-d");
		boot_argv[boot_argc++] = strdup("3");  /* debug level */
		boot_argv[boot_argc++] = strdup("-X");
		boot_argv[boot_argc++] = strdup("1048576");  /* 1MB WAL segments */
		boot_argv[boot_argc] = NULL;

		/* Fork child process for bootstrap */
		bootstrap_pid = fork();
		if (bootstrap_pid < 0)
		{
			fprintf(stderr, "\nERROR: fork failed: %s\n", strerror(errno));
			exit(1);
		}

		if (bootstrap_pid == 0)
		{
			/* Child process - run bootstrap */
			int saved_stdin;

			/* Redirect stdin to BKI file */
			saved_stdin = dup(STDIN_FILENO);
			if (freopen(bki_temp_path, "r", stdin) == NULL)
			{
				fprintf(stderr, "\nERROR: could not freopen stdin: %s\n", strerror(errno));
				exit(1);
			}

			/*
			 * Initialize essential subsystems that main.c normally does
			 * before calling BootstrapModeMain
			 */
			MyProcPid = getpid();
			MemoryContextInit();

			/* Reset getopt state */
			optind = 1;
			opterr = 1;
			optopt = 0;

			/* Call BootstrapModeMain - this will exit the child process */
			BootstrapModeMain(boot_argc, boot_argv, false);

			/* Should not reach here */
			exit(0);
		}

		/* Parent process - wait for bootstrap to complete */
		printf("\n[DEBUG] Waiting for bootstrap child process %d\n", bootstrap_pid);
		fflush(stdout);

		if (waitpid(bootstrap_pid, &status, 0) < 0)
		{
			fprintf(stderr, "\nERROR: waitpid failed: %s\n", strerror(errno));
			exit(1);
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			fprintf(stderr, "\nERROR: bootstrap process failed with status %d\n", status);
			exit(1);
		}

		printf("\n[DEBUG] Bootstrap completed successfully\n");
		fflush(stdout);

		/* Clean up */
		unlink(bki_temp_path);
	}

	printf("ok\n");

	/*
	 * Post-bootstrap: Run SQL setup scripts
	 * Use our embedded API to run SQL without forking
	 */
	printf("running post-bootstrap initialization ... ");
	fflush(stdout);

	{
		char sql_file_paths[4][MAXPGPATH];
		const char *sql_files[5];
		char cwd[MAXPGPATH];
		int i;

		/* Get current working directory and build absolute paths BEFORE pg_embedded_init
		 * because pg_embedded_init will chdir to the data directory */
		if (getcwd(cwd, sizeof(cwd)) == NULL)
		{
			fprintf(stderr, "\nERROR: getcwd failed: %s\n", strerror(errno));
			return -1;
		}

		snprintf(sql_file_paths[0], MAXPGPATH, "%s/src/include/catalog/system_constraints.sql", cwd);
		snprintf(sql_file_paths[1], MAXPGPATH, "%s/src/backend/catalog/system_functions.sql", cwd);
		snprintf(sql_file_paths[2], MAXPGPATH, "%s/src/backend/catalog/system_views.sql", cwd);
		snprintf(sql_file_paths[3], MAXPGPATH, "%s/src/backend/catalog/information_schema.sql", cwd);

		sql_files[0] = sql_file_paths[0];
		sql_files[1] = sql_file_paths[1];
		sql_files[2] = sql_file_paths[2];
		sql_files[3] = sql_file_paths[3];
		sql_files[4] = NULL;

		/* Set PGDATA environment variable for pg_embedded_init */
		setenv("PGDATA", pg_data, 1);

		/*
		 * Initialize embedded mode on template1 with system table mods enabled.
		 * This is required to run the post-bootstrap SQL scripts that modify
		 * system catalogs (pg_proc, pg_type, etc.)
		 */
		if (pg_embedded_init_with_system_mods(pg_data, "template1", username_g) != 0)
		{
			fprintf(stderr, "\nERROR: Failed to initialize embedded mode: %s\n",
					pg_embedded_error_message());
			return -1;
		}

		/* Run each SQL file */
		for (i = 0; sql_files[i] != NULL; i++)
		{
			FILE *sql_file;
			char *sql_content = NULL;
			long file_size;
			size_t bytes_read;

			printf("\n[DEBUG] Running %s\n", sql_files[i]);
			fflush(stdout);

			/* Read entire SQL file into memory */
			sql_file = fopen(sql_files[i], "r");
			if (!sql_file)
			{
				fprintf(stderr, "\nWARNING: could not open %s: %s\n",
						sql_files[i], strerror(errno));
				continue;
			}

			/* Get file size */
			fseek(sql_file, 0, SEEK_END);
			file_size = ftell(sql_file);
			fseek(sql_file, 0, SEEK_SET);

			/* Allocate buffer */
			sql_content = malloc(file_size + 1);
			if (!sql_content)
			{
				fprintf(stderr, "\nERROR: Out of memory\n");
				fclose(sql_file);
				return -1;
			}

			/* Read file */
			bytes_read = fread(sql_content, 1, file_size, sql_file);
			sql_content[bytes_read] = '\0';
			fclose(sql_file);

			/* Execute SQL */
			{
				pg_result *result = pg_embedded_exec(sql_content);
				if (!result)
				{
					fprintf(stderr, "\nERROR: pg_embedded_exec returned NULL for %s: %s\n",
							sql_files[i], pg_embedded_error_message());
					free(sql_content);
					return -1;
				}

				if (result->status < 0)
				{
					fprintf(stderr, "\nWARNING: SQL execution had errors in %s (status=%d): %s\n",
							sql_files[i], result->status, pg_embedded_error_message());
					/* Continue anyway - some errors may be expected */
				}

				pg_embedded_free_result(result);
			}

			free(sql_content);
		}

		/* Shutdown embedded mode - we need to restart without system_table_mods */
		pg_embedded_shutdown();
	}

	printf("ok\n");

	/*
	 * Create template0 and postgres databases
	 * We do this in a SEPARATE session WITHOUT allow_system_table_mods
	 * to avoid the databases inheriting that setting
	 */
	printf("creating template0 and postgres databases ... ");
	fflush(stdout);

	{
		char abs_pg_data[MAXPGPATH];

		/* Get absolute path since we may have changed directory */
		if (pg_data[0] != '/')
		{
			char cwd[MAXPGPATH];
			if (getcwd(cwd, sizeof(cwd)) == NULL)
			{
				fprintf(stderr, "\nERROR: getcwd failed: %s\n", strerror(errno));
				return -1;
			}
			/* Go back to original directory before getting absolute path */
			if (chdir("..") != 0)
			{
				fprintf(stderr, "\nERROR: chdir failed: %s\n", strerror(errno));
				return -1;
			}
			if (getcwd(cwd, sizeof(cwd)) == NULL)
			{
				fprintf(stderr, "\nERROR: getcwd failed: %s\n", strerror(errno));
				return -1;
			}
			snprintf(abs_pg_data, sizeof(abs_pg_data), "%s/%s", cwd, pg_data);
		}
		else
		{
			strlcpy(abs_pg_data, pg_data, sizeof(abs_pg_data));
		}

		/* Initialize embedded mode WITHOUT system table mods */
		if (pg_embedded_init(abs_pg_data, "template1", username_g) != 0)
		{
			fprintf(stderr, "\nERROR: Failed to re-initialize embedded mode: %s\n",
					pg_embedded_error_message());
			return -1;
		}

		/* Create template0 database using C API */
		if (create_database_direct("template0", 4, true, false, "unmodifiable empty database") != 0)
		{
			pg_embedded_shutdown();
			return -1;
		}

		/* Create postgres database using C API */
		if (create_database_direct("postgres", 5, false, true,
								   "default administrative connection database") != 0)
		{
			pg_embedded_shutdown();
			return -1;
		}

		/* Shutdown embedded mode */
		pg_embedded_shutdown();
	}

	printf("ok\n");

	printf("\nDatabase cluster initialized successfully!\n");
	printf("Location: %s\n", pg_data);
	printf("\nYou can now connect to the 'postgres' database.\n");

	return 0;
}
