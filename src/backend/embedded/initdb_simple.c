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
#include <unistd.h>

#include "bootstrap/bootstrap.h"
#include "common/file_perm.h"
#include "common/restricted_token.h"
#include "common/username.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#include "initdb_embedded.h"

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
	 * This is the core phase that creates the system catalogs
	 */
	printf("running bootstrap script ... ");
	fflush(stdout);

	{
		/* For now, just create a minimal bootstrap setup */
		char *boot_argv[10];
		int boot_argc = 0;
		FILE *bki_src, *bki_dest;
		char line[8192];
		const char *bki_src_path = "src/include/catalog/postgres.bki";
		const char *bki_temp_path = "/tmp/pg_bootstrap.bki";
		int saved_stdin;

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

		/* Redirect stdin to BKI file */
		saved_stdin = dup(STDIN_FILENO);
		if (freopen(bki_temp_path, "r", stdin) == NULL)
		{
			fprintf(stderr, "\nERROR: could not freopen stdin: %s\n", strerror(errno));
			exit(1);
		}

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

		printf("\n[DEBUG] Calling BootstrapModeMain with argc=%d\n", boot_argc);
		fflush(stdout);

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

		/* Call BootstrapModeMain */
		BootstrapModeMain(boot_argc, boot_argv, false);

		printf("\n[DEBUG] BootstrapModeMain returned\n");
		fflush(stdout);

		/* Restore stdin */
		fclose(stdin);
		dup2(saved_stdin, STDIN_FILENO);
		close(saved_stdin);

		/* Clean up */
		unlink(bki_temp_path);
	}

	printf("ok\n");

	printf("\nBootstrap phase completed successfully!\n");
	printf("Database cluster initialized at %s\n", pg_data);
	printf("\nNote: Post-bootstrap SQL not yet implemented.\n");
	printf("The database has system catalogs but no template1 database yet.\n");

	return 0;
}
