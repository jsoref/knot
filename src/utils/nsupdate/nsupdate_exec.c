/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils/nsupdate/nsupdate_exec.h"
#include "utils/common/params.h"
#include "utils/common/msg.h"
#include "utils/common/exec.h"
#include "utils/common/netio.h"
#include "utils/common/token.h"
#include "common/mem.h"
#include "libknot/errcode.h"
#include "libknot/descriptor.h"
#include "common/strlcpy.h"
#include "libknot/common.h"
#include "libknot/libknot.h"
#include "libknot/dnssec/random.h"

/* Declarations of cmd parse functions. */
typedef int (*cmd_handle_f)(const char *lp, nsupdate_params_t *params);
int cmd_add(const char* lp, nsupdate_params_t *params);
int cmd_answer(const char* lp, nsupdate_params_t *params);
int cmd_class(const char* lp, nsupdate_params_t *params);
int cmd_debug(const char* lp, nsupdate_params_t *params);
int cmd_del(const char* lp, nsupdate_params_t *params);
int cmd_gsstsig(const char* lp, nsupdate_params_t *params);
int cmd_key(const char* lp, nsupdate_params_t *params);
int cmd_local(const char* lp, nsupdate_params_t *params);
int cmd_nxdomain(const char *lp, nsupdate_params_t *params);
int cmd_nxrrset(const char *lp, nsupdate_params_t *params);
int cmd_oldgsstsig(const char* lp, nsupdate_params_t *params);
int cmd_origin(const char* lp, nsupdate_params_t *params);
int cmd_prereq(const char* lp, nsupdate_params_t *params);
int cmd_realm(const char* lp, nsupdate_params_t *params);
int cmd_send(const char* lp, nsupdate_params_t *params);
int cmd_server(const char* lp, nsupdate_params_t *params);
int cmd_show(const char* lp, nsupdate_params_t *params);
int cmd_ttl(const char* lp, nsupdate_params_t *params);
int cmd_update(const char* lp, nsupdate_params_t *params);
int cmd_yxdomain(const char *lp, nsupdate_params_t *params);
int cmd_yxrrset(const char *lp, nsupdate_params_t *params);
int cmd_zone(const char* lp, nsupdate_params_t *params);

/* Sorted list of commands.
 * This way we could identify command byte-per-byte and
 * cancel early if the next is lexicographically greater.
 */
const char* cmd_array[] = {
	"\x3" "add",
	"\x6" "answer",
	"\x5" "class",         /* {classname} */
	"\x5" "debug",
	"\x3" "del",
	"\x6" "delete",
	"\x7" "gsstsig",
	"\x3" "key",           /* {name} {secret} */
	"\x5" "local",         /* {address} [port] */
	"\x8" "nxdomain",
	"\x7" "nxrrset",
	"\xa" "oldgsstsig",
	"\x6" "origin",        /* {name} */
	"\x6" "prereq",        /* (nx|yx)(domain|rrset) {domain-name} ... */
	"\x5" "realm",         /* {[realm_name]} */
	"\x4" "send",
	"\x6" "server",        /* {servername} [port] */
	"\x4" "show",
	"\x3" "ttl",           /* {seconds} */
	"\x6" "update",        /* (add|delete) {domain-name} ... */
	"\x8" "yxdomain",
	"\x7" "yxrrset",
	"\x4" "zone",          /* {zonename} */
	NULL
};

cmd_handle_f cmd_handle[] = {
	cmd_add,
	cmd_answer,
	cmd_class,
	cmd_debug,
	cmd_del,
	cmd_del,         /* delete/del synonyms */
	cmd_gsstsig,
	cmd_key,
	cmd_local,
	cmd_nxdomain,
	cmd_nxrrset,
	cmd_oldgsstsig,
	cmd_origin,
	cmd_prereq,
	cmd_realm,
	cmd_send,
	cmd_server,
	cmd_show,
	cmd_ttl,
	cmd_update,
	cmd_yxdomain,
	cmd_yxrrset,
	cmd_zone,
};

/* {prereq} command table. */
const char* pq_array[] = {
	"\x8" "nxdomain",
	"\x7" "nxrrset",
	"\x8" "yxdomain",
	"\x7" "yxrrset",
	NULL
};

enum {
	PQ_NXDOMAIN = 0,
	PQ_NXRRSET,
	PQ_YXDOMAIN,
	PQ_YXRRSET,
	UP_ADD,
	UP_DEL
};

/* RR parser flags */
enum {
	PARSE_NODEFAULT = 1 << 0, /* Do not fill defaults. */
	PARSE_NAMEONLY  = 1 << 1, /* Parse only name. */
	PARSE_NOTTL     = 1 << 2  /* Ignore TTL item. */
};

static bool dname_isvalid(const char *lp)
{
	knot_dname_t *dn = knot_dname_from_str(lp);
	if (dn == NULL) {
		return false;
	}
	knot_dname_free(&dn, NULL);
	return true;
}

/* This is probably redundant, but should be a bit faster so let's keep it. */
static int parse_full_rr(zs_scanner_t *s, const char* lp)
{
	if (zs_scanner_parse(s, lp, lp + strlen(lp), true) < 0) {
		return KNOT_EPARSEFAIL;
	}

	/* Class must not differ from specified. */
	if (s->r_class != s->default_class) {
		char cls_s[16] = {0};
		knot_rrclass_to_string(s->default_class, cls_s, sizeof(cls_s));
		ERR("class mismatch: '%s'\n", cls_s);
		return KNOT_EPARSEFAIL;
	}

	return KNOT_EOK;
}

static int parse_partial_rr(zs_scanner_t *s, const char *lp, unsigned flags)
{
	int ret = KNOT_EOK;
	bool fqdn = true;

	/* Extract owner. */
	size_t len = strcspn(lp, SEP_CHARS);
	char *owner_str = calloc(1, len + 2); // 2 ~ ('.' + '\0')
	memcpy(owner_str, lp, len);

	/* Make dname FQDN if it isn't. */
	if (owner_str[len - 1] != '.') {
		owner_str[len++] = '.';
		fqdn = false;
	}

	knot_dname_t *owner = knot_dname_from_str(owner_str);
	free(owner_str);
	if (owner == NULL) {
		return KNOT_EPARSEFAIL;
	}

	s->r_owner_length = knot_dname_size(owner);
	memcpy(s->r_owner, owner, s->r_owner_length);
	knot_dname_free(&owner, NULL);

	/* Append origin if not FQDN. */
	if (!fqdn) {
		s->r_owner_length--;
		memcpy(s->r_owner + s->r_owner_length, s->zone_origin,
		       s->zone_origin_length);
		s->r_owner_length += s->zone_origin_length;
	}

	lp = tok_skipspace(lp + len);

	/* Initialize */
	s->r_type = KNOT_RRTYPE_ANY;
	s->r_class = s->default_class;
	s->r_data_length = 0;
	if (flags & PARSE_NODEFAULT) {
		s->r_ttl = 0;
	} else {
		s->r_ttl = s->default_ttl;
	}

	/* Parse only name? */
	if (flags & PARSE_NAMEONLY) {
		if (*lp != '\0') {
			WARN("ignoring input data: '%s'\n", lp);
		}
		return KNOT_EOK;
	}

	/* Now there could be [ttl] [class] [type [data...]]. */
	char *np = NULL;
	long ttl = strtol(lp, &np, 10);
	if (ttl >= 0 && np && (*np == '\0' || isspace((unsigned char)(*np)))) {
		DBG("%s: parsed ttl=%lu\n", __func__, ttl);
		if (flags & PARSE_NOTTL) {
			WARN("ignoring TTL value: '%ld'\n", ttl);
		} else {
			s->r_ttl = ttl;
		}
		lp = tok_skipspace(np);
	}

	uint16_t num;
	char *buff = NULL;
	char *cls = NULL;
	char *type = NULL;

	/* Try to find class. */
	len = strcspn(lp, SEP_CHARS);
	if (len > 0) {
		buff = strndup(lp, len);
	}

	if (knot_rrclass_from_string(buff, &num) == 0) {
		/* Class must not differ from specified. */
		if (num != s->default_class) {
			ERR("class mismatch: '%s'\n", buff);
			free(buff);
			return KNOT_EPARSEFAIL;
		}
		cls = buff;
		buff = NULL;
		s->r_class = num;
		DBG("%s: parsed class=%u '%s'\n", __func__, s->r_class, cls);
		lp = tok_skipspace(lp + len);
	}

	/* Try to parser type. */
	if (cls != NULL) {
		len = strcspn(lp, SEP_CHARS);
		if (len > 0) {
			buff = strndup(lp, len);
		}
	}
	if (knot_rrtype_from_string(buff, &num) == 0) {
		type = buff;
		buff = NULL;
		s->r_type = num;
		DBG("%s: parsed type=%u '%s'\n", __func__, s->r_type, type);
		lp = tok_skipspace(lp + len);
	}

	free(buff);

	/* Remainder */
	if (*lp == '\0') {
		free(cls);
		free(type);
		return ret; /* No RDATA */
	}

	/* Need to parse rdata, synthetize input. */
	char *rr = sprintf_alloc(" %u IN %s %s\n", s->r_ttl, type, lp);
	free(cls);
	free(type);
	if (rr == NULL || zs_scanner_parse(s, rr, rr + strlen(rr), true) < 0) {
		ret = KNOT_EPARSEFAIL;
	}
	free(rr);

	return ret;
}

static srv_info_t *parse_host(const char *lp, const char* default_port)
{
	/* Extract server address. */
	srv_info_t *srv = NULL;
	size_t len = strcspn(lp, SEP_CHARS);
	char *addr = strndup(lp, len);
	if (!addr) return NULL;
	DBG("%s: parsed addr: %s\n", __func__, addr);

	/* Store port/service if present. */
	lp = tok_skipspace(lp + len);
	if (*lp == '\0') {
		srv = srv_info_create(addr, default_port);
		free(addr);
		return srv;
	}

	len = strcspn(lp, SEP_CHARS);
	char *port = strndup(lp, len);
	if (!port) {
		free(addr);
		return NULL;
	}
	DBG("%s: parsed port: %s\n", __func__, port);

	/* Create server struct. */
	srv = srv_info_create(addr, port);
	free(addr);
	free(port);
	return srv;
}

/* Append parsed RRSet to list. */
static int rr_list_append(zs_scanner_t *s, list_t *target_list, knot_mm_ctx_t *mm)
{
	knot_rrset_t *rr = knot_rrset_new(s->r_owner, s->r_type, s->r_class,
	                                  NULL);
	if (!rr) {
		DBG("%s: failed to create rrset\n", __func__);
		return KNOT_ENOMEM;
	}

	/* Create RDATA. */
	int ret = knot_rrset_add_rdata(rr, s->r_data, s->r_data_length,
	                               s->r_ttl, NULL);
	if (ret != KNOT_EOK) {
		DBG("%s: failed to set rrset from wire - %s\n",
		    __func__, knot_strerror(ret));
		knot_rrset_free(&rr, NULL);
		return ret;
	}

	if (ptrlist_add(target_list, rr, mm) == NULL) {
		knot_rrset_free(&rr, NULL);
		return KNOT_ENOMEM;
	}

	return KNOT_EOK;
}

/*! \brief Write RRSet list to packet section. */
static int rr_list_to_packet(knot_pkt_t *dst, list_t *list)
{
	assert(dst != NULL);
	assert(list != NULL);

	int ret = KNOT_EOK;
	ptrnode_t *node = NULL;
	WALK_LIST(node, *list) {
		ret = knot_pkt_put(dst, KNOT_COMPR_HINT_NONE,
		                   (knot_rrset_t *)node->d, 0);
		if (ret != KNOT_EOK) {
			break;
		}
	}

	return ret;
}

/*! \brief Build UPDATE query. */
static int build_query(nsupdate_params_t *params)
{
	/* Clear old query. */
	knot_pkt_t *query = params->query;
	knot_pkt_clear(query);

	/* Write question. */
	knot_wire_set_id(query->wire, knot_random_uint16_t());
	knot_wire_set_opcode(query->wire, KNOT_OPCODE_UPDATE);
	knot_dname_t *qname = knot_dname_from_str(params->zone);
	int ret = knot_pkt_put_question(query, qname, params->class_num,
	                                params->type_num);
	knot_dname_free(&qname, NULL);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Now, PREREQ => ANSWER section. */
	ret = knot_pkt_begin(query, KNOT_ANSWER);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Write PREREQ. */
	ret = rr_list_to_packet(query, &params->prereq_list);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Now, UPDATE data => AUTHORITY section. */
	ret = knot_pkt_begin(query, KNOT_AUTHORITY);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Write UPDATE data. */
	return rr_list_to_packet(query, &params->update_list);
}

static int pkt_sendrecv(nsupdate_params_t *params)
{
	net_t net;
	int   ret;

	ret = net_init(params->srcif,
	               params->server,
	               get_iptype(params->ip),
	               get_socktype(params->protocol, KNOT_RRTYPE_SOA),
	               params->wait,
	               &net);
	if (ret != KNOT_EOK) {
		return -1;
	}

	ret = net_connect(&net);
	DBG("%s: send_msg = %d\n", __func__, net.sockfd);
	if (ret != KNOT_EOK) {
		return -1;
	}

	ret = net_send(&net, params->query->wire, params->query->size);
	if (ret != KNOT_EOK) {
		net_close(&net);
		net_clean(&net);
		return -1;
	}

	/* Clear response buffer. */
	knot_pkt_clear(params->answer);

	/* Wait for reception. */
	int rb = net_receive(&net, params->answer->wire, params->answer->max_size);
	DBG("%s: receive_msg = %d\n", __func__, rb);
	if (rb <= 0) {
		net_close(&net);
		net_clean(&net);
		return -1;
	} else {
		params->answer->size = rb;
	}

	net_close(&net);
	net_clean(&net);

	return rb;
}

static int nsupdate_process_line(char *lp, int len, void *arg)
{
	nsupdate_params_t *params = (nsupdate_params_t *)arg;

	/* Remove trailing white space chars. */
	for (int i = len - 1; i >= 0; i--) {
		if (isspace((unsigned char)lp[i]) == 0) {
			break;
		}
		lp[i] = '\0';
	}

	/* Check for empty line or comment. */
	if (lp[0] == '\0' || lp[0] == ';') {
		return KNOT_EOK;
	}

	int ret = tok_find(lp, cmd_array);
	if (ret < 0) {
		return ret; /* Syntax error - do nothing. */
	}

	const char *cmd = cmd_array[ret];
	const char *val = tok_skipspace(lp + TOK_L(cmd));
	ret = cmd_handle[ret](val, params);
	if (ret != KNOT_EOK) {
		DBG("operation '%s' failed (%s) on line '%s'\n",
		    TOK_S(cmd), knot_strerror(ret), lp);
	}

	return ret;
}

static int nsupdate_process(nsupdate_params_t *params, FILE *fp)
{
	/* Process lines. */
	return tok_process_lines(fp, nsupdate_process_line, params);
}

int nsupdate_exec(nsupdate_params_t *params)
{
	if (!params) {
		return KNOT_EINVAL;
	}

	int ret = KNOT_EOK;

	/* If not file specified, use stdin. */
	if (EMPTY_LIST(params->qfiles)) {
		return nsupdate_process(params, stdin);
	}

	/* Read from each specified file. */
	ptrnode_t *n = NULL;
	WALK_LIST(n, params->qfiles) {
		const char *filename = (const char*)n->d;
		if (strcmp(filename, "-") == 0) {
			ret = nsupdate_process(params, stdin);
			continue;
		}
		FILE *fp = fopen(filename, "r");
		if (!fp) {
			ERR("failed to open '%s': %s\n",
			    filename, strerror(errno));
			return KNOT_ERROR;
		}
		ret = nsupdate_process(params, fp);
		fclose(fp);
	}

	return ret;
}

int cmd_update(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* update is optional token, next add|del|delete */
	int bp = tok_find(lp, cmd_array);
	if (bp < 0) return bp; /* Syntax error. */

	/* allow only specific tokens */
	cmd_handle_f *h = cmd_handle;
	if (h[bp] != cmd_add && h[bp] != cmd_del) {
		ERR("unexpected token '%s' after 'update', allowed: '%s'\n",
		    lp, "{add|del|delete}");
		return KNOT_EPARSEFAIL;
	}

	return h[bp](tok_skipspace(lp + TOK_L(cmd_array[bp])), params);
}

int cmd_add(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	if (parse_full_rr(params->parser, lp) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}

	return rr_list_append(params->parser, &params->update_list, &params->mm);
}

int cmd_del(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	zs_scanner_t *rrp = params->parser;
	if (parse_partial_rr(rrp, lp, PARSE_NODEFAULT) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}

	/* Check owner name. */
	if (rrp->r_owner_length == 0) {
		ERR("failed to parse owner name '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	rrp->r_ttl = 0; /* Set TTL = 0 when deleting. */

	/* When deleting whole RRSet, use ANY class */
	if (rrp->r_data_length == 0) {
		rrp->r_class = KNOT_CLASS_ANY;
	} else {
		rrp->r_class = KNOT_CLASS_NONE;
	}

	return rr_list_append(rrp, &params->update_list, &params->mm);
}

int cmd_class(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	uint16_t cls;

	if (knot_rrclass_from_string(lp, &cls) != 0) {
		ERR("failed to parse class '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	} else {
		params->class_num = cls;
		params->parser->default_class = params->class_num;
	}

	return KNOT_EOK;
}

int cmd_ttl(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	uint32_t ttl = 0;

	if (params_parse_num(lp, &ttl) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}

	return nsupdate_set_ttl(params, ttl);
}

int cmd_debug(const char* lp, nsupdate_params_t *params)
{
	KNOT_UNUSED(params);
	DBG("%s: lp='%s'\n", __func__, lp);

	msg_enable_debug(1);
	return KNOT_EOK;
}

int cmd_nxdomain(const char *lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	zs_scanner_t *s = params->parser;
	int ret = parse_partial_rr(s, lp, PARSE_NODEFAULT|PARSE_NAMEONLY);
	if (ret != KNOT_EOK) {
		return ret;
	}

	s->r_ttl = 0;
	s->r_class = KNOT_CLASS_NONE;

	return rr_list_append(s, &params->prereq_list, &params->mm);
}

int cmd_yxdomain(const char *lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	zs_scanner_t *s = params->parser;
	int ret = parse_partial_rr(s, lp, PARSE_NODEFAULT|PARSE_NAMEONLY);
	if (ret != KNOT_EOK) {
		return ret;
	}

	s->r_ttl = 0;
	s->r_class = KNOT_CLASS_ANY;

	return rr_list_append(s, &params->prereq_list, &params->mm);
}

int cmd_nxrrset(const char *lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	zs_scanner_t *s = params->parser;
	int ret = parse_partial_rr(s, lp, PARSE_NOTTL);
	if (ret != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}

	/* Check owner name. */
	if (s->r_owner_length == 0) {
		ERR("failed to parse prereq owner name '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	s->r_ttl = 0;
	s->r_class = KNOT_CLASS_NONE;

	return rr_list_append(s, &params->prereq_list, &params->mm);
}

int cmd_yxrrset(const char *lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	zs_scanner_t *s = params->parser;
	int ret = parse_partial_rr(s, lp, PARSE_NOTTL);
	if (ret != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}

	/* Check owner name. */
	if (s->r_owner_length == 0) {
		ERR("failed to parse prereq owner name '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	s->r_ttl = 0;
	if (s->r_data_length > 0) {
		s->r_class = KNOT_CLASS_IN;
	} else {
		s->r_class = KNOT_CLASS_ANY;
	}

	return rr_list_append(s, &params->prereq_list, &params->mm);
}

int cmd_prereq(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Scan prereq specifier ([ny]xrrset|[ny]xdomain) */
	int ret = KNOT_EOK;
	int prereq_type = tok_find(lp, pq_array);
	if (prereq_type < 0) return prereq_type; /* Syntax error. */

	const char *tok = pq_array[prereq_type];
	DBG("%s: type %s\n", __func__, TOK_S(tok));
	lp = tok_skipspace(lp + TOK_L(tok));

	switch(prereq_type) {
	case PQ_NXDOMAIN:
		ret = cmd_nxdomain(lp, params);
		break;
	case PQ_YXDOMAIN:
		ret = cmd_yxdomain(lp, params);
		break;
	case PQ_NXRRSET:
		ret = cmd_nxrrset(lp, params);
		break;
	case PQ_YXRRSET:
		ret = cmd_yxrrset(lp, params);
		break;
	default:
		ret = KNOT_ERROR;
	}

	return ret;
}

int cmd_send(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	DBG("sending packet\n");

	/* Build query packet. */
	int ret = build_query(params);
	if (ret != KNOT_EOK) {
		ERR("failed to build UPDATE message - %s\n", knot_strerror(ret));
		return ret;
	}

	sign_context_t sign_ctx;
	memset(&sign_ctx, '\0', sizeof(sign_context_t));

	/* Sign if key specified. */
	if (params->key_params.name) {
		ret = sign_packet(params->query, &sign_ctx, &params->key_params);
		if (ret != KNOT_EOK) {
			ERR("failed to sign UPDATE message - %s\n",
			    knot_strerror(ret));
			return ret;
		}
	}

	int rb = 0;
	/* Send/recv message (1 try + N retries). */
	int tries = 1 + params->retries;
	for (; tries > 0; --tries) {
		rb = pkt_sendrecv(params);
		if (rb > 0) break;
	}

	/* Check Send/recv result. */
	if (rb <= 0) {
		free_sign_context(&sign_ctx);
		return KNOT_ECONNREFUSED;
	}

	/* Parse response. */
	ret = knot_pkt_parse(params->answer, 0);
	if (ret != KNOT_EOK) {
		ERR("failed to parse response, %s\n", knot_strerror(ret));
		free_sign_context(&sign_ctx);
		return ret;
	}

	/* Check signature if expected. */
	if (params->key_params.name) {
		ret = verify_packet(params->answer, &sign_ctx, &params->key_params);
		free_sign_context(&sign_ctx);
		if (ret != KNOT_EOK) { /* Collect TSIG error. */
			fprintf(stderr, "%s: %s\n", "; TSIG error with server",
				knot_strerror(ret));
			return ret;
		}
	}

	/* Free RRSet lists. */
	nsupdate_reset(params);

	/* Check return code. */
	knot_lookup_table_t *rcode;
	int rc = knot_wire_get_rcode(params->answer->wire);
	DBG("%s: received rcode=%d\n", __func__, rc);
	rcode = knot_lookup_by_id(knot_rcode_names, rc);
	if (rcode && rcode->id > KNOT_RCODE_NOERROR) {
		ERR("update failed: %s\n", rcode->name);
	}

	return KNOT_EOK;
}

int cmd_zone(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Check zone name. */
	if (!dname_isvalid(lp)) {
		ERR("failed to parse zone '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	free(params->zone);
	params->zone = strdup(lp);

	return KNOT_EOK;
}

int cmd_server(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Parse host. */
	srv_info_t *srv = parse_host(lp, params->server->service);

	/* Enqueue. */
	if (!srv) return KNOT_ENOMEM;

	srv_info_free(params->server);
	params->server = srv;

	return KNOT_EOK;
}

int cmd_local(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Parse host. */
	srv_info_t *srv = parse_host(lp, "0");

	/* Enqueue. */
	if (!srv) return KNOT_ENOMEM;

	srv_info_free(params->srcif);
	params->srcif = srv;

	return KNOT_EOK;
}

int cmd_show(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Show current packet. */
	if (!params->query) return KNOT_EOK;
	printf("Update query:\n");
	build_query(params);
	print_packet(params->query, NULL, 0, -1, 0, false, &params->style);
	printf("\n");
	return KNOT_EOK;
}

int cmd_answer(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Show current answer. */
	if (!params->answer) return KNOT_EOK;
	printf("\nAnswer:\n");
	print_packet(params->answer, NULL, 0, -1, 0, true, &params->style);
	return KNOT_EOK;
}

int cmd_key(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	char *kstr = strdup(lp); /* Convert to default format. */
	if (!kstr) return KNOT_ENOMEM;

	int ret = KNOT_EOK;
	size_t len = strcspn(lp, SEP_CHARS);
	if(kstr[len] == '\0') {
		ERR("command 'key' without {secret} specified\n");
		ret = KNOT_EINVAL;
	} else {
		// override existing key
		knot_free_key_params(&params->key_params);

		kstr[len] = ':'; /* Replace ' ' with ':' sep */
		ret = params_parse_tsig(kstr, &params->key_params);
	}

	free(kstr);
	return ret;
}

int cmd_origin(const char* lp, nsupdate_params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);

	/* Check zone name. */
	if (!dname_isvalid(lp)) {
		ERR("failed to parse zone '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	return nsupdate_set_origin(params, lp);
}

/*
 *   Not implemented.
 */

int cmd_gsstsig(const char* lp, nsupdate_params_t *params)
{
	KNOT_UNUSED(params);
	DBG("%s: lp='%s'\n", __func__, lp);

	return KNOT_ENOTSUP;
}

int cmd_oldgsstsig(const char* lp, nsupdate_params_t *params)
{
	KNOT_UNUSED(params);
	DBG("%s: lp='%s'\n", __func__, lp);

	return KNOT_ENOTSUP;
}

int cmd_realm(const char* lp, nsupdate_params_t *params)
{
	KNOT_UNUSED(params);
	DBG("%s: lp='%s'\n", __func__, lp);

	return KNOT_ENOTSUP;
}
