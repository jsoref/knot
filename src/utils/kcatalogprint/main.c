/*  Copyright (C) 2021 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include "knot/catalog/catalog_db.h"
#include "utils/common/params.h"
#include "utils/common/util_conf.h"

#define PROGRAM_NAME	"kcatalogprint"

static void print_help(void)
{
	printf("Usage: %s [-c | -C | -D <path>] [parameters]\n"
	       "\n"
	       "Parameters:\n"
	       " -c, --config <file> Path to a textual configuration file.\n"
	       "                      (default %s)\n"
	       " -C, --confdb <dir>  Path to a configuration database directory.\n"
	       "                      (default %s)\n"
	       " -D, --dir <path>    Path to a catalog database directory, use default configuration.\n"
	       " -h, --help          Print the program help.\n"
	       " -V, --version       Print the program version.\n",
	       PROGRAM_NAME, CONF_DEFAULT_FILE, CONF_DEFAULT_DBDIR);
}

static void print_dname(const knot_dname_t *d)
{
	knot_dname_txt_storage_t tmp;
	knot_dname_to_str(tmp, d, sizeof(tmp));
	printf("%s  ", tmp);
}

static int catalog_print_cb(const knot_dname_t *mem, const knot_dname_t *ow,
                            const knot_dname_t *cz, const char *group, void *ctx)
{
	print_dname(mem);
	print_dname(ow);
	print_dname(cz);
	printf("%s\n", group);
	(*(ssize_t *)ctx)++;
	return KNOT_EOK;
}

static void catalog_print(catalog_t *cat)
{
	ssize_t total = 0;

	printf(";; <member zone> <record owner> <catalog zone> <group>\n");

	if (cat != NULL) {
		int ret = catalog_open(cat);
		if (ret == KNOT_EOK) {
			ret = catalog_apply(cat, NULL, catalog_print_cb, &total, false);
		}
		if (ret != KNOT_EOK) {
			printf("Catalog print failed (%s)\n", knot_strerror(ret));
			return;
		}
	}

	printf("Total records: %zd\n", total);
}

int main(int argc, char *argv[])
{
	struct option opts[] = {
		{ "config",  required_argument, NULL, 'c' },
		{ "confdb",  required_argument, NULL, 'C' },
		{ "dir",     required_argument, NULL, 'D' },
		{ "help",    no_argument,       NULL, 'h' },
		{ "version", no_argument,       NULL, 'V' },
		{ NULL }
	};

	int opt = 0;
	while ((opt = getopt_long(argc, argv, "c:C:D:hV", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (util_conf_init_file(optarg) != KNOT_EOK) {
				return EXIT_FAILURE;
			}
			break;
		case 'C':
			if (util_conf_init_confdb(optarg) != KNOT_EOK) {
				return EXIT_FAILURE;
			}
			break;
		case 'D':
			if (util_conf_init_justdb("catalog-db", optarg) != KNOT_EOK) {
				return EXIT_FAILURE;
			}
			break;
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'V':
			print_version(PROGRAM_NAME);
			return EXIT_SUCCESS;
		default:
			print_help();
			return EXIT_FAILURE;
		}
	}

	// Backward compatibility.
	if (argc - optind > 0) {
		fprintf(stderr, "Warning: obsolete parameter specified\n");
		if (util_conf_init_justdb("catalog-db", argv[optind]) != KNOT_EOK) {
			return EXIT_FAILURE;
		}
		optind++;
	}

	if (util_conf_init_default() != KNOT_EOK) {
		return EXIT_FAILURE;
	}

	catalog_t c = { { 0 } };

	char *db = conf_db(conf(), C_CATALOG_DB);
	catalog_init(&c, db, 0); // mapsize grows automatically
	free(db);
	catalog_print(&c);
	if (catalog_deinit(&c) != KNOT_EOK) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
