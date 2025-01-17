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

#include <sys/stat.h>
#include <unistd.h>

#include "utils/common/util_conf.h"

#include "contrib/string.h"
#include "knot/conf/conf.h"
#include "libknot/attribute.h"
#include "utils/common/msg.h"

bool util_conf_initialized(void)
{
	return (conf() != NULL);
}

int util_conf_init_confdb(const char *confdb)
{
	if (util_conf_initialized()) {
		ERR("Configuration already initialized.\n");
		return KNOT_ESEMCHECK;
	}

	size_t max_conf_size = (size_t)CONF_MAPSIZE * 1024 * 1024;

	conf_flag_t flags = CONF_FNOHOSTNAME | CONF_FOPTMODULES;
	if (confdb != NULL) {
		flags |= CONF_FREADONLY;
	}

	conf_t *new_conf = NULL;
	int ret = conf_new(&new_conf, conf_schema, confdb, max_conf_size, flags);
	if (ret != KNOT_EOK) {
		ERR("failed opening configuration database %s (%s)\n",
		    (confdb == NULL ? "" : confdb), knot_strerror(ret));
	} else {
		conf_update(new_conf, CONF_UPD_FNONE);
	}
	return ret;
}

int util_conf_init_file(const char *conffile)
{
	int ret = util_conf_init_confdb(NULL);
	if (ret != KNOT_EOK) {
		return ret;
	}

	ret = conf_import(conf(), conffile, true, false);
	if (ret != KNOT_EOK) {
		ERR("failed opening configuration file %s (%s)\n",
		    conffile, knot_strerror(ret));
	}
	return ret;
}

int util_conf_init_justdb(const char *db_type, const char *db_path)
{
	int ret = util_conf_init_confdb(NULL);
	if (ret != KNOT_EOK) {
		return ret;
	}

	char *conf_str = sprintf_alloc("database:\n"
	                               "  storage: .\n"
	                               "  %s: \"%s\"\n", db_type, db_path);
	if (conf_str == NULL) {
		return KNOT_ENOMEM;
	}

	ret = conf_import(conf(), conf_str, false, false);
	free(conf_str);
	if (ret != KNOT_EOK) {
		ERR("failed creating temporary configuration (%s)\n", knot_strerror(ret));
	}
	return ret;
}

int util_conf_init_default(void)
{
	struct stat st;
	if (util_conf_initialized()) {
		return KNOT_EOK;
	} else if (conf_db_exists(CONF_DEFAULT_DBDIR)) {
		return util_conf_init_confdb(CONF_DEFAULT_DBDIR);
	} else if (stat(CONF_DEFAULT_FILE, &st) == 0) {
		return util_conf_init_file(CONF_DEFAULT_FILE);
	} else {
		ERR("couldn't initialize configuration, please provide -c, -C, or -D option\n");
		return KNOT_EINVAL;
	}
}

void util_update_privileges(void)
{
	int uid, gid;
	if (conf_user(conf(), &uid, &gid) != KNOT_EOK) {
		return;
	}

	// Just try to alter process privileges if different from configured.
	_unused_ int unused;
	if ((gid_t)gid != getgid()) {
		unused = setregid(gid, gid);
	}
	if ((uid_t)uid != getuid()) {
		unused = setreuid(uid, uid);
	}
}

void util_conf_deinit(void)
{
	conf_free(conf());
}
