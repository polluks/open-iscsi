/*
 * iSCSI Discovery Database Library
 *
 * Copyright (C) 2004 Dmitry Yusupov, Alex Aizman
 * maintained by open-iscsi@@googlegroups.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#define DB_DBM_HSEARCH 1
#include <db.h>

#include "idbm.h"
#include "log.h"

#define LOCK_EX 2    /* Exclusive lock.  */
#define LOCK_UN 8    /* Unlock.  */
#define HIDE	0    /* Hide parameter when print. */
#define SHOW	1    /* Show parameter when print. */
#define MASKED	2    /* Show "stars" instead of real value when print */

#define __recinfo_str(_key, _info, _rec, _name, _show, _n) do { \
	_info[_n].type = TYPE_STR; \
	strncpy(_info[_n].name, _key, NAME_MAXVAL); \
	if (strlen(_rec->_name)) \
		strncpy(_info[_n].value, _rec->_name, VALUE_MAXVAL); \
	else \
		strcpy(_info[_n].value, "<empty>"); \
	_info[_n].data = &_rec->_name; \
	_info[_n].visible = _show; \
	_n++; \
} while(0)

#define __recinfo_int(_key, _info, _rec, _name, _show, _n) do { \
	_info[_n].type = TYPE_INT; \
	strncpy(_info[_n].name, _key, NAME_MAXVAL); \
	snprintf(_info[_n].value, VALUE_MAXVAL, "%d", _rec->_name); \
	_info[_n].data = &_rec->_name; \
	_info[_n].visible = _show; \
	_n++; \
} while(0)

#define __recinfo_int_o2(_key,_info,_rec,_name,_show,_op0,_op1,_n) do { \
	_info[_n].type = TYPE_INT_O; \
	strncpy(_info[_n].name, _key, NAME_MAXVAL); \
	if (_rec->_name == 0) strncpy(_info[_n].value, _op0, VALUE_MAXVAL); \
	if (_rec->_name == 1) strncpy(_info[_n].value, _op1, VALUE_MAXVAL); \
	_info[_n].data = &_rec->_name; \
	_info[_n].visible = _show; \
	_info[_n].opts[0] = _op0; \
	_info[_n].opts[1] = _op1; \
	_info[_n].numopts = 2; \
	_n++; \
} while(0)

#define __recinfo_int_o3(_key,_info,_rec,_name,_show,_op0,_op1,_op2,_n)do{ \
	__recinfo_int_o2(_key,_info,_rec,_name,_show,_op0,_op1,_n); _n--; \
	if (_rec->_name == 2) strncpy(_info[_n].value, _op2, VALUE_MAXVAL); \
	_info[_n].opts[2] = _op2; \
	_info[_n].numopts = 3; \
	_n++; \
} while(0)

#define __recinfo_int_o4(_key,_info,_rec,_name,_show,_op0,_op1,_op2,_op3,_n)do{\
	__recinfo_int_o3(_key,_info,_rec,_name,_show,_op0,_op1,_op2,_n); _n--; \
	if (_rec->_name == 3) strncpy(_info[_n].value, _op3, VALUE_MAXVAL); \
	_info[_n].opts[3] = _op3; \
	_info[_n].numopts = 4; \
	_n++; \
} while(0)

static int
idbm_uniq_id(char *name)
{
	unsigned long h = 0, g;
	static int M = 0x100000;

	while (*name) {
		h = ( h << 4 ) + *name++;
		if ((g = h & 0xF0000000L))
			h ^= g >> 23;

		h &= ~g;
	}
	return h % M;
}

char *
get_iscsi_initiatorname(char *pathname)
{
	FILE *f = NULL;
	int c;
	char *line, buffer[1024];
	char *name = NULL;

	if (!pathname) {
		log_error("No pathname to load InitiatorName from");
		return NULL;
	}

	/* get the InitiatorName */
	if ((f = fopen(pathname, "r"))) {
		while ((line = fgets(buffer, sizeof (buffer), f))) {

			while (line && isspace(c = *line))
				line++;

			if (strncmp(line, "InitiatorName=", 14) == 0) {
				char *end = line + 14;

				/* the name is everything up to the first
				 * bit of whitespace
				 */
				while (*end && (!isspace(c = *end)))
					end++;

				if (isspace(c = *end))
					*end = '\0';

				if (end > line + 14)
					name = strdup(line + 14);
			}
		}
		fclose(f);
		if (!name) {
			log_error(
			       "an InitiatorName is required, but "
			       "was not found in %s", pathname);
			return NULL;
		} else {
			log_debug(5, "InitiatorName=%s", name);
		}
		return name;
	} else {
		log_error("cannot open InitiatorName configuration file %s",
			 pathname);
		return NULL;
	}
}

static int
idbm_lock(DBM *dbm)
{
#ifndef DB_DBM_HSEARCH
	if (flock(dbm->dbm_dirf, LOCK_EX) == -1 ||
	    flock(dbm->dbm_pagf, LOCK_EX) == -1)
		return 1;
#else
	if (flock(dbm_dirfno(dbm), LOCK_EX) == -1)
		return 1;
#endif
	return 0;
}

static void
idbm_unlock(DBM *dbm)
{
#ifndef DB_DBM_HSEARCH
	flock(dbm->dbm_dirf, LOCK_UN);
	flock(dbm->dbm_pagf, LOCK_UN);
#else
	flock(dbm_dirfno(dbm), LOCK_UN);
#endif
}

static void
idbm_update_discovery(discovery_rec_t *rec, discovery_rec_t *newrec)
{
#define __update_rec_int(r, n, fld) \
	if (n->fld) r->fld = n->fld
#define __update_rec_str(r, n, fld, len) \
	if (strlen(n->fld)) strncpy(r->fld, n->fld, len)

	__update_rec_int(rec, newrec, startup);
	__update_rec_int(rec, newrec, type);
	__update_rec_str(rec, newrec, u.sendtargets.address, 16);
	__update_rec_int(rec, newrec, u.sendtargets.port);
	__update_rec_int(rec, newrec, u.sendtargets.continuous);
	__update_rec_int(rec, newrec, u.sendtargets.send_async_text);
	__update_rec_int(rec, newrec, u.sendtargets.auth.authmethod);
	__update_rec_str(rec, newrec,u.sendtargets.auth.username,
			 AUTH_STR_MAX_LEN);
	__update_rec_str(rec, newrec, u.sendtargets.auth.password,
			 AUTH_STR_MAX_LEN);
	__update_rec_int(rec, newrec,
			 u.sendtargets.auth.password_length);
	__update_rec_str(rec, newrec,u.sendtargets.auth.username_in,
			 AUTH_STR_MAX_LEN);
	__update_rec_str(rec, newrec, u.sendtargets.auth.password_in,
			 AUTH_STR_MAX_LEN);
	__update_rec_int(rec, newrec,
			 u.sendtargets.auth.password_length_in);
	__update_rec_int(rec, newrec,
		 u.sendtargets.cnx_timeo.login_timeout);
	__update_rec_int(rec, newrec,
		 u.sendtargets.cnx_timeo.auth_timeout);
	__update_rec_int(rec, newrec,
		 u.sendtargets.cnx_timeo.active_timeout);
	__update_rec_int(rec, newrec,
		 u.sendtargets.cnx_timeo.idle_timeout);
	__update_rec_int(rec, newrec,
		 u.sendtargets.cnx_timeo.ping_timeout);
}

static char*
idbm_hash_discovery(discovery_rec_t *rec)
{
	char *hash = malloc(HASH_MAXLEN);

	if (!hash) {
		log_error("out of memory on hash allocation");
		return NULL;
	}

	if (rec->type == DISCOVERY_TYPE_SENDTARGETS) {
		snprintf(hash, HASH_MAXLEN, "%s:%d",
			rec->u.sendtargets.address,
			rec->u.sendtargets.port);
	}

	return hash;
}

static void
idbm_update_node(node_rec_t *rec, node_rec_t *newrec)
{
	int i;

#define __update_rec_int(r, n, fld) \
	if (n->fld) r->fld = n->fld
#define __update_rec_str(r, n, fld, len) \
	if (strlen(n->fld)) strncpy(r->fld, n->fld, len)

	/* update rec */
	__update_rec_str(rec, newrec, name, TARGET_NAME_MAXLEN);
	__update_rec_int(rec, newrec, tpgt);
	__update_rec_int(rec, newrec, startup);

	/* update rec->session */
	__update_rec_int(rec, newrec, session.initial_cmdsn);
	__update_rec_str(rec, newrec, session.auth.username,
			 AUTH_STR_MAX_LEN);
	__update_rec_str(rec, newrec, session.auth.password,
			 AUTH_STR_MAX_LEN);
	__update_rec_int(rec, newrec, session.auth.password_length);
	__update_rec_str(rec, newrec, session.auth.username_in,
			 AUTH_STR_MAX_LEN);
	__update_rec_str(rec, newrec, session.auth.password_in,
			 AUTH_STR_MAX_LEN);
	__update_rec_int(rec, newrec, session.auth.password_length_in);
	__update_rec_int(rec, newrec, session.timeo.replacement_timeout);
	__update_rec_int(rec, newrec, session.err_timeo.abort_timeout);
	__update_rec_int(rec, newrec, session.err_timeo.reset_timeout);
	__update_rec_int(rec, newrec, session.iscsi.protocol);
	__update_rec_int(rec, newrec, session.iscsi.InitialR2T);
	__update_rec_int(rec, newrec, session.iscsi.ImmediateData);
	__update_rec_int(rec, newrec, session.iscsi.FirstBurstLength);
	__update_rec_int(rec, newrec, session.iscsi.MaxBurstLength);
	__update_rec_int(rec, newrec, session.iscsi.DefaultTime2Wait);
	__update_rec_int(rec, newrec, session.iscsi.DefaultTime2Retain);

	for (i=0; i < ISCSI_CNX_MAX; i++) {
		/* update rec->cnx[i] */
		__update_rec_str(rec, newrec, cnx[i].address, 16);
		__update_rec_int(rec, newrec, cnx[i].port);
		__update_rec_int(rec, newrec, cnx[i].startup);
		__update_rec_int(rec, newrec, cnx[i].tcp.window_size);
		__update_rec_int(rec, newrec, cnx[i].tcp.type_of_service);
		__update_rec_int(rec, newrec, cnx[i].timeo.login_timeout);
		__update_rec_int(rec, newrec, cnx[i].timeo.auth_timeout);
		__update_rec_int(rec, newrec, cnx[i].timeo.active_timeout);
		__update_rec_int(rec, newrec, cnx[i].timeo.idle_timeout);
		__update_rec_int(rec, newrec, cnx[i].timeo.ping_timeout);
		__update_rec_int(rec, newrec,
				 cnx[i].iscsi.MaxRecvDataSegmentLength);
		__update_rec_int(rec, newrec, cnx[i].iscsi.HeaderDigest);
		__update_rec_int(rec, newrec, cnx[i].iscsi.DataDigest);
	}
}

static char*
idbm_hash_node(discovery_rec_t *drec, node_rec_t *nrec)
{
	char *hash = malloc(HASH_MAXLEN);

	if (!hash) {
		log_error("out of memory on hash allocation");
		return NULL;
	}

	if (drec == NULL) {
		snprintf(hash, HASH_MAXLEN, "%s:%d,%d",
			nrec->cnx[0].address,
			nrec->cnx[0].port,
			nrec->tpgt);
		return hash;
	}

	if (drec->type == DISCOVERY_TYPE_SENDTARGETS) {
		snprintf(hash, HASH_MAXLEN, "%s:%d#%s:%d,%d",
			drec->u.sendtargets.address,
			drec->u.sendtargets.port,
			nrec->cnx[0].address,
			nrec->cnx[0].port,
			nrec->tpgt);
	}

	return hash;
}

static void*
idbm_read(DBM *dbm, char *hash)
{
	datum key, data;

	key.dptr = hash;
	key.dsize = HASH_MAXLEN;

	data = dbm_fetch(dbm, key);
	if (data.dsize > 0) {
		return data.dptr;
	}

	log_debug(7, "key '%s' not found", hash);
	return NULL;
}

static void*
idbm_read_with_id(DBM *dbm, int rec_id)
{
	datum key, data;

	(void)idbm_lock(dbm);
	for (key=dbm_firstkey(dbm); key.dptr != NULL; key=dbm_nextkey(dbm)) {
		data = dbm_fetch(dbm, key);
		if (idbm_uniq_id(key.dptr) == rec_id) {
			idbm_unlock(dbm);
			return data.dptr;
		}
	}
	idbm_unlock(dbm);
	return NULL;
}

static int
idbm_write(DBM *dbm, void *rec, int size, char *hash)
{
	datum key, data;

	key.dptr = hash;
	key.dsize = HASH_MAXLEN;

	data.dptr = rec;
	data.dsize = size;

	if (dbm_store(dbm, key, data, DBM_REPLACE)) {
		log_error("can not write record with hash-key '%s'", hash);
		return 1;
	}

	return 0;
}

static void
idbm_recinfo_discovery(discovery_rec_t *r, recinfo_t *ri)
{
	int num = 0;

	__recinfo_int_o2("discovery.startup", ri, r, startup, SHOW,
			"manual", "automatic", num);
	__recinfo_int_o3("discovery.type", ri, r, type, SHOW,
			"sendtargets", "slp", "isns", num);
	if (r->type == DISCOVERY_TYPE_SENDTARGETS) {
		__recinfo_str("discovery.sendtargets.address", ri, r,
			u.sendtargets.address, SHOW, num);
		__recinfo_int("discovery.sendtargets.port", ri, r,
			u.sendtargets.port, SHOW, num);
		__recinfo_int("discovery.sendtargets.continuous", ri, r,
			u.sendtargets.continuous, SHOW, num);
		__recinfo_int("discovery.sendtargets.send_async_text", ri, r,
			u.sendtargets.send_async_text, SHOW, num);
		__recinfo_int_o2("discovery.sendtargets.auth.authmethod", ri, r,
			u.sendtargets.auth.authmethod,
			SHOW, "None", "CHAP", num);
		__recinfo_str("discovery.sendtargets.auth.username", ri, r,
			u.sendtargets.auth.username, SHOW, num);
		__recinfo_str("discovery.sendtargets.auth.password", ri, r,
			u.sendtargets.auth.password, MASKED, num);
		__recinfo_str("discovery.sendtargets.auth.username_in", ri, r,
			u.sendtargets.auth.username_in, SHOW, num);
		__recinfo_str("discovery.sendtargets.auth.password_in", ri, r,
			u.sendtargets.auth.password_in, MASKED, num);
		__recinfo_int("discovery.sendtargets.timeo.login_timeout",ri, r,
			u.sendtargets.cnx_timeo.login_timeout,
			SHOW, num);
		__recinfo_int("discovery.sendtargets.timeo.auth_timeout", ri, r,
			u.sendtargets.cnx_timeo.auth_timeout,
			SHOW, num);
		__recinfo_int("discovery.sendtargets.timeo.active_timeout",ri,r,
			u.sendtargets.cnx_timeo.active_timeout,
			SHOW, num);
		__recinfo_int("discovery.sendtargets.timeo.idle_timeout", ri, r,
			u.sendtargets.cnx_timeo.idle_timeout,
			SHOW, num);
		__recinfo_int("discovery.sendtargets.timeo.ping_timeout", ri, r,
			u.sendtargets.cnx_timeo.ping_timeout,
			SHOW, num);
	}
}

static void
idbm_recinfo_node(node_rec_t *r, recinfo_t *ri)
{
	int num = 0, i;

	__recinfo_str("node.name", ri, r, name, SHOW, num);
	__recinfo_int("node.tpgt", ri, r, tpgt, SHOW, num);
	__recinfo_int("node.active_cnx", ri, r, active_cnx, SHOW, num);
	__recinfo_int_o2("node.startup", ri, r, startup,
			SHOW, "manual", "automatic", num);
	__recinfo_int("node.session.initial_cmdsn", ri, r,
		      session.initial_cmdsn, SHOW, num);
	__recinfo_str("node.session.auth.username", ri, r,
		      session.auth.username, SHOW, num);
	__recinfo_str("node.session.auth.password", ri, r,
		      session.auth.password, MASKED, num);
	__recinfo_str("node.session.auth.username_in", ri, r,
		      session.auth.username_in, SHOW, num);
	__recinfo_str("node.session.auth.password_in", ri, r,
		      session.auth.password_in, MASKED, num);
	__recinfo_int("node.session.timeo.replacement_timeout", ri, r,
		      session.timeo.replacement_timeout,
		      SHOW, num);
	__recinfo_int("node.session.err_timeo.abort_timeout", ri, r,
		      session.err_timeo.abort_timeout,
		      SHOW, num);
	__recinfo_int("node.session.err_timeo.reset_timeout", ri, r,
		      session.err_timeo.reset_timeout,
		      SHOW, num);
	__recinfo_int_o2("node.session.iscsi.InitialR2T", ri, r,
			 session.iscsi.InitialR2T, SHOW,
			"No", "Yes", num);
	__recinfo_int_o2("node.session.iscsi.ImmediateData",
			 ri, r, session.iscsi.ImmediateData, SHOW,
			"No", "Yes", num);
	__recinfo_int("node.session.iscsi.FirstBurstLength", ri, r,
		      session.iscsi.FirstBurstLength, SHOW, num);
	__recinfo_int("node.session.iscsi.MaxBurstLength", ri, r,
		      session.iscsi.MaxBurstLength, SHOW, num);
	__recinfo_int("node.session.iscsi.DefaultTime2Retain", ri, r,
		      session.iscsi.DefaultTime2Wait, SHOW, num);
	__recinfo_int("node.session.iscsi.DefaultTime2Wait", ri, r,
		      session.iscsi.DefaultTime2Retain, SHOW, num);
	__recinfo_int("node.session.iscsi.MaxConnections", ri, r,
		      session.iscsi.MaxConnections, SHOW, num);

	for (i=0; i < r->active_cnx; i++) {
		char key[NAME_MAXVAL];
		sprintf(key, "node.cnx[%d].address", i);
		__recinfo_str(key, ri, r, cnx[i].address, SHOW, num);
		sprintf(key, "node.cnx[%d].port", i);
		__recinfo_int(key, ri, r, cnx[i].port, SHOW, num);
		sprintf(key, "node.cnx[%d].startup", i);
		__recinfo_int_o2(key, ri, r, cnx[i].startup, SHOW,
				 "manual", "automatic", num);
		sprintf(key, "node.cnx[%d].tcp.window_size", i);
		__recinfo_int(key, ri, r, cnx[i].tcp.window_size, SHOW, num);
		sprintf(key, "node.cnx[%d].tcp.type_of_service", i);
		__recinfo_int(key, ri, r, cnx[i].tcp.type_of_service,
				SHOW, num);
		sprintf(key, "node.cnx[%d].timeo.login_timeout", i);
		__recinfo_int(key, ri, r, cnx[i].timeo.login_timeout,
				SHOW, num);
		sprintf(key, "node.cnx[%d].timeo.auth_timeout", i);
		__recinfo_int(key, ri, r, cnx[i].timeo.auth_timeout,
				SHOW, num);
		sprintf(key, "node.cnx[%d].timeo.active_timeout", i);
		__recinfo_int(key, ri, r, cnx[i].timeo.active_timeout,
				SHOW, num);
		sprintf(key, "node.cnx[%d].timeo.idle_timeout", i);
		__recinfo_int(key, ri, r, cnx[i].timeo.idle_timeout,
				SHOW, num);
		sprintf(key, "node.cnx[%d].timeo.ping_timeout", i);
		__recinfo_int(key, ri, r, cnx[i].timeo.ping_timeout,
				SHOW, num);
		sprintf(key, "node.cnx[%d].iscsi.MaxRecvDataSegmentLength", i);
		__recinfo_int(key, ri, r,
			cnx[i].iscsi.MaxRecvDataSegmentLength, SHOW, num);
		sprintf(key, "node.cnx[%d].iscsi.HeaderDigest", i);
		__recinfo_int_o4(key, ri, r, cnx[i].iscsi.HeaderDigest, SHOW,
				 "None", "CRC32C", "CRC32C,None",
				 "None,CRC32C", num);
		sprintf(key, "node.cnx[%d].iscsi.DataDigest", i);
		__recinfo_int_o4(key, ri, r, cnx[i].iscsi.DataDigest, SHOW,
				 "None", "CRC32C", "CRC32C,None",
				 "None,CRC32C", num);
	}
}

static recinfo_t*
idbm_recinfo_alloc(int max_keys)
{
	recinfo_t *info;

	info = malloc(sizeof(recinfo_t)*max_keys);
	if (!info)
		return NULL;
	memset(info, 0, sizeof(recinfo_t)*max_keys);
	return info;
}

static DBM*
idbm_open(char *filename, int flags)
{
	DBM *dbm;

	if (flags & O_CREAT) {
		char *dirname, *ptr;

		dirname = strdup(filename);
		if (dirname && (ptr = strrchr(dirname, '/'))) {
			*ptr = '\0';
		} else if (!dirname)
			return NULL;

		if (access(dirname, F_OK) != 0) {
			if (mkdir(dirname, 0755) != 0) {
				free(dirname);
				log_error("mkdir '%s' error", dirname);
				return NULL;
			}
		}
		free(dirname);
	}

	/* Now open the database */
	dbm = dbm_open(filename, flags, 0666);
	if (!dbm) {
		log_error("discovery DB '%s' open failed", filename);
		return NULL;
	}

	return dbm;
}

static void
idbm_close(DBM *dbm)
{
	dbm_close(dbm);
}

#define PRINT_TYPE_DISCOVERY	0
#define PRINT_TYPE_NODE		1
static void
idbm_print(int type, void *rec)
{
	int i;
	recinfo_t *info;

	info = idbm_recinfo_alloc(MAX_KEYS);
	if (!info)
		return;

	if (type == PRINT_TYPE_DISCOVERY) {
		idbm_recinfo_discovery((discovery_rec_t*)rec, info);
	} else {
		idbm_recinfo_node((node_rec_t*)rec, info);
	}

	for (i=0; i<MAX_KEYS; i++) {
		if (!info[i].visible)
			continue;
		if (info[i].visible == MASKED) {
			if (*(char*)info[i].data)
				printf("%s = ********\n", info[i].name);
			else
				printf("%s = %s\n", info[i].name,
					info[i].value);
		} else
			printf("%s = %s\n", info[i].name, info[i].value);
	}

	free(info);
}

static int
idbm_print_type(idbm_t *db, int type, int rec_id)
{
	int found = 0;
	datum key, data;
	DBM *dbm = type == PRINT_TYPE_DISCOVERY ? db->discdb : db->nodedb;

	(void)idbm_lock(dbm);

	for (key=dbm_firstkey(dbm); key.dptr != NULL; key=dbm_nextkey(dbm)) {
		data = dbm_fetch(dbm, key);
		if (rec_id < 0) {
			if (type == PRINT_TYPE_DISCOVERY) {
				discovery_rec_t *rec = (discovery_rec_t*)
								data.dptr;
				if (rec->type == DISCOVERY_TYPE_SENDTARGETS) {
					printf("[%06x] %s:%d via sendtargets\n",
						rec->id,
						rec->u.sendtargets.address,
						rec->u.sendtargets.port);
				}
			} else if (type == PRINT_TYPE_NODE) {
				node_rec_t *rec = (node_rec_t*)data.dptr;
				printf("[%06x] %s:%d,%d %s\n",
					rec->id,
					rec->cnx[0].address,
					rec->cnx[0].port,
					rec->tpgt,
					rec->name);
			}
			found++;
		} else if (rec_id == idbm_uniq_id(key.dptr)) {
			idbm_print(type, data.dptr);
			found++;
		}
	}

	idbm_unlock(dbm);
	return found;
}

static void
idbm_node_setup_defaults(node_rec_t *rec)
{
	int i;

	memset(rec, 0, sizeof(node_rec_t));

	rec->active_cnx = 1; /* at least one connection must exist */
	rec->session.initial_cmdsn = 0;
	rec->session.auth.authmethod = 0;
	rec->session.err_timeo.abort_timeout = 10;
	rec->session.err_timeo.reset_timeout = 30;
	rec->session.timeo.replacement_timeout = 0;
	rec->session.iscsi.InitialR2T = 0;
	rec->session.iscsi.ImmediateData = 1;
	rec->session.iscsi.FirstBurstLength = 256 * 1024;
	rec->session.iscsi.MaxBurstLength = (16 * 1024 * 1024) - 1024;
	rec->session.iscsi.DefaultTime2Wait = 0;
	rec->session.iscsi.DefaultTime2Retain = 0;
	rec->session.iscsi.MaxConnections = 1;

	for (i=0; i<ISCSI_CNX_MAX; i++) {
		rec->cnx[i].startup = 0;
		rec->cnx[i].tcp.window_size = 256 * 1024;
		rec->cnx[i].tcp.type_of_service = 0;
		rec->cnx[i].timeo.login_timeout=15;
		rec->cnx[i].timeo.auth_timeout = 45;
		rec->cnx[i].timeo.active_timeout=5;
		rec->cnx[i].timeo.idle_timeout = 60;
		rec->cnx[i].timeo.ping_timeout = 5;
		rec->cnx[i].iscsi.MaxRecvDataSegmentLength = 128 * 1024;
		rec->cnx[i].iscsi.HeaderDigest = CONFIG_DIGEST_PREFER_OFF;
		rec->cnx[i].iscsi.DataDigest = CONFIG_DIGEST_PREFER_OFF;
	}

}

static void
idbm_discovery_setup_defaults(discovery_rec_t *rec, discovery_type_e type)
{
	memset(rec, 0, sizeof(discovery_rec_t));

	rec->startup = ISCSI_STARTUP_MANUAL;
	rec->type = type;
	if (type == DISCOVERY_TYPE_SENDTARGETS) {
		rec->u.sendtargets.continuous = 0;
		rec->u.sendtargets.send_async_text = 0;
		rec->u.sendtargets.auth.authmethod = 0;
		rec->u.sendtargets.cnx_timeo.login_timeout=15;
		rec->u.sendtargets.cnx_timeo.auth_timeout = 45;
		rec->u.sendtargets.cnx_timeo.active_timeout=5;
		rec->u.sendtargets.cnx_timeo.idle_timeout = 60;
		rec->u.sendtargets.cnx_timeo.ping_timeout = 5;
	} else if (type == DISCOVERY_TYPE_SLP) {
		rec->u.slp.interfaces = NULL;
		rec->u.slp.scopes = NULL;
		rec->u.slp.poll_interval = 5 * 60;	/* 5 minutes */
		rec->u.slp.auth.authmethod = 0;
		rec->u.slp.auth.password_length = 0;
		rec->u.slp.auth.password_length_in = 0;
	} else if (type == DISCOVERY_TYPE_ISNS) {
		/* to be implemented */
	}
}

static void
idbm_recinfo_config(recinfo_t *info, FILE *f)
{
	char name[NAME_MAXVAL];
	char value[VALUE_MAXVAL];
	char *line, *nl, buffer[2048];
	int line_number = 0;
	int c, i;

	fseek(f, 0, SEEK_SET);

	/* process the config file */
	do {
		line = fgets(buffer, sizeof (buffer), f);
		line_number++;
		if (!line)
			continue;

		/* skip leading whitespace */
		while (isspace(c = *line))
			line++;

		/* strip trailing whitespace, including the newline.
		 * anything that needs the whitespace must be quoted.
		 */
		nl = line + strlen(line) - 1;
		if (*nl == '\n') {
			do {
				*nl = '\0';
				nl--;
			} while (isspace(c = *nl));
		} else {
			log_warning("config file line %d too long",
			       line_number);
			continue;
		}

		/* process any non-empty, non-comment lines */
		if (!*line || *line == '#')
			continue;

		/* parse name */
		i=0; nl = line; *name = 0;
		while (*nl && !isspace(c = *nl) && *nl != '=') {
			*(name+i) = *nl; i++; nl++;
		}
		if (!*nl) {
			log_warning("config file line %d do not has value",
			       line_number);
			continue;
		}
		*(name+i)=0; nl++;
		/* skip after-name traling spaces */
		while (*nl && isspace(c = *nl)) nl++;
		if (*nl && *nl != '=') {
			log_warning("config file line %d has not '=' sepa",
			       line_number);
			continue;
		}
		/* skip '=' sepa */
		nl++;
		/* skip after-sepa traling spaces */
		while (*nl && isspace(c = *nl)) nl++;
		if (!*nl) {
			log_warning("config file line %d do not has value",
			       line_number);
			continue;
		}
		/* parse value */
		i=0; *value = 0;
		while (*nl) {
			*(value+i) = *nl; i++; nl++;
		}
		*(value+i) = 0;

		for (i=0; i<MAX_KEYS; i++) {
			if (!info[i].visible)
				continue;
			if (!strcmp(name, info[i].name)) {
				int j;
				log_debug(7, "updated '%s', old value %s "
					  "new value %s", name,
					  info[i].value, value);
				/* parse recinfo by type */
				if (info[i].type == TYPE_INT) {
					*(int*)info[i].data =
						strtoul(value, NULL, 10);
					break;
				} else if (info[i].type == TYPE_STR) {
					strncpy((char*)info[i].data,
						value, VALUE_MAXLEN);
					break;
				}
				for (j=0; j<info[i].numopts; j++) {
					if (!strcmp(value, info[i].opts[j])) {
						*(int*)info[i].data = j;
						break;
					}
				}
				if (j != info[i].numopts)
					break;
				log_warning("config file line %d contains "
					    "unknown value format '%s' for "
					    "parameter name '%s'",
					    line_number, value, name);
				break;
			}
		}
	} while (line);
}

static void
idbm_sync_config(idbm_t *db)
{
	FILE *f = NULL;

	/* in case of no configuration file found we just
	 * initialize default node and default discovery records
	 * from hard-coded default values */
	idbm_node_setup_defaults(&db->nrec);
	idbm_discovery_setup_defaults(&db->drec_st, DISCOVERY_TYPE_SENDTARGETS);
	idbm_discovery_setup_defaults(&db->drec_slp, DISCOVERY_TYPE_SLP);
	idbm_discovery_setup_defaults(&db->drec_isns, DISCOVERY_TYPE_ISNS);

	f = fopen(db->configfile, "r");
	if (!f) {
		log_debug(1, "cannot open configuration file %s",
				db->configfile);
		return;
	}

	log_debug(5, "updating defaults from '%s'", db->configfile);

	idbm_recinfo_discovery(&db->drec_st, db->dinfo_st);
	idbm_recinfo_discovery(&db->drec_slp, db->dinfo_slp);
	idbm_recinfo_discovery(&db->drec_isns, db->dinfo_isns);
	idbm_recinfo_node(&db->nrec, db->ninfo);

	idbm_recinfo_config(db->dinfo_st, f);
	idbm_recinfo_config(db->dinfo_slp, f);
	idbm_recinfo_config(db->dinfo_isns, f);
	idbm_recinfo_config(db->ninfo, f);

	/* update password lengths */
	if (*db->drec_st.u.sendtargets.auth.password)
		db->drec_st.u.sendtargets.auth.password_length =
			strlen(db->drec_st.u.sendtargets.auth.password);
	if (*db->drec_st.u.sendtargets.auth.password_in)
		db->drec_st.u.sendtargets.auth.password_length_in =
		     strlen(db->drec_st.u.sendtargets.auth.password_in);
	if (*db->drec_slp.u.slp.auth.password)
		db->drec_slp.u.slp.auth.password_length =
			strlen(db->drec_slp.u.slp.auth.password);
	if (*db->drec_slp.u.slp.auth.password_in)
		db->drec_slp.u.slp.auth.password_length_in =
			strlen(db->drec_slp.u.slp.auth.password_in);
	if (*db->nrec.session.auth.password)
		db->nrec.session.auth.password_length =
			strlen(db->nrec.session.auth.password);
	if (*db->nrec.session.auth.password_in)
		db->nrec.session.auth.password_length_in =
			strlen(db->nrec.session.auth.password_in);

	fclose(f);
}

static char*
idbm_id2hash(DBM *dbm, int rec_id)
{
	datum key, data;

	(void)idbm_lock(dbm);
	for (key=dbm_firstkey(dbm); key.dptr != NULL; key=dbm_nextkey(dbm)) {
		data = dbm_fetch(dbm, key);
		if (idbm_uniq_id(key.dptr) == rec_id) {
			idbm_unlock(dbm);
			return strdup(key.dptr);
		}
	}
	idbm_unlock(dbm);
	return NULL;
}

int
idbm_print_discovery(idbm_t *db, int rec_id)
{
	return idbm_print_type(db, PRINT_TYPE_DISCOVERY, rec_id);
}

int
idbm_print_node(idbm_t *db, int rec_id)
{
	return idbm_print_type(db, PRINT_TYPE_NODE, rec_id);
}

int
idbm_print_nodes(idbm_t *db, discovery_rec_t *drec)
{
	int found = 0;
	char *hash;
	datum key, data;
	DBM *dbm = db->nodedb;

	if (!(hash = idbm_hash_discovery(drec)))
		return found;

	(void)idbm_lock(dbm);

	for (key=dbm_firstkey(dbm); key.dptr != NULL; key=dbm_nextkey(dbm)) {
		data = dbm_fetch(dbm, key);
		if (strstr(key.dptr, hash)) {
			node_rec_t *rec = (node_rec_t*)data.dptr;
			printf("[%06x] %s:%d,%d %s\n",
			        rec->id,
				rec->cnx[0].address,
				rec->cnx[0].port,
				rec->tpgt,
				rec->name);
			found++;
		}
	}

	idbm_unlock(dbm);
	free(hash);
	return found;
}

int
idbm_discovery_read(idbm_t *db, int rec_id, discovery_rec_t *out_rec)
{
	discovery_rec_t *rec;

	if ((rec = (discovery_rec_t*)idbm_read_with_id(db->discdb, rec_id))) {
		memcpy(out_rec, rec, sizeof(discovery_rec_t));
		return 0;
	}
	return 1;
}

int
idbm_node_read(idbm_t *db, int rec_id, node_rec_t *out_rec)
{
	node_rec_t *rec;

	if ((rec = (node_rec_t*)idbm_read_with_id(db->nodedb, rec_id))) {
		memcpy(out_rec, rec, sizeof(node_rec_t));
		return 0;
	}
	return 1;
}

int
idbm_node_write(idbm_t *db, int rec_id, node_rec_t *rec)
{
	char *hash;
	DBM *dbm = db->nodedb;

	hash = idbm_id2hash(dbm, rec_id);
	if (!hash)
		return 1;

	if (idbm_lock(dbm)) {
		free(hash);
		return 1;
	}

	if (idbm_write(dbm, rec, sizeof(node_rec_t), hash)) {
		idbm_unlock(dbm);
		free(hash);
		return 1;
	}

	idbm_unlock(dbm);

	free(hash);
	return 0;
}

int
idbm_add_discovery(idbm_t *db, discovery_rec_t *newrec)
{
	char *hash;
	discovery_rec_t *rec;
	DBM *dbm = db->discdb;

	if (!(hash = idbm_hash_discovery(newrec))) {
		return 1;
	}

	if (idbm_lock(dbm)) {
		free(hash);
		return 1;
	}

	if ((rec = idbm_read(dbm, hash))) {
		log_debug(7, "updating existing DB record");
		idbm_update_discovery(rec, newrec);
	} else {
		log_debug(7, "adding new DB record");
	}

	newrec->id = idbm_uniq_id(hash);
	if (idbm_write(dbm, newrec, sizeof(discovery_rec_t), hash)) {
		idbm_unlock(dbm);
		free(hash);
		return 1;
	}

	idbm_unlock(dbm);
	free(hash);
	return 0;
}

int
idbm_add_node(idbm_t *db, discovery_rec_t *drec, node_rec_t *newrec)
{
	char *hash;
	node_rec_t *rec;
	DBM *dbm = db->nodedb;

	if (!(hash = idbm_hash_node(drec, newrec))) {
		return 1;
	}

	if (idbm_lock(dbm)) {
		free(hash);
		return 1;
	}

	if ((rec = idbm_read(dbm, hash))) {
		log_debug(7, "updating existing DB record");
		idbm_update_node(rec, newrec);
	} else {
		log_debug(7, "adding new DB record");
	}

	newrec->id = idbm_uniq_id(hash);
	if (idbm_write(dbm, newrec, sizeof(node_rec_t), hash)) {
		idbm_unlock(dbm);
		free(hash);
		return 1;
	}

	idbm_unlock(dbm);
	free(hash);
	return 0;
}

discovery_rec_t*
idbm_new_discovery(idbm_t *db, char *ip, int port,
			discovery_type_e type, char *info)
{
	char *ptr, *newinfo;
	discovery_rec_t *drec;
	node_rec_t *nrec;

	/* sync default configuration */
	idbm_sync_config(db);

	/* allocate new discovery record and initialize with defaults */
	drec = malloc(sizeof(discovery_rec_t));
	if (!drec) {
		log_error("out of memory on discovery record allocation");
		return NULL;
	}
	if (drec->type == DISCOVERY_TYPE_SENDTARGETS) {
		memcpy(drec, &db->drec_st, sizeof(discovery_rec_t));
	} else if (drec->type == DISCOVERY_TYPE_SLP) {
		memcpy(drec, &db->drec_slp, sizeof(discovery_rec_t));
	} else if (drec->type == DISCOVERY_TYPE_ISNS) {
		memcpy(drec, &db->drec_isns, sizeof(discovery_rec_t));
	}

	/* allocate new node record and initialize with defaults */
	nrec = malloc(sizeof(node_rec_t));
	if (!nrec) {
		log_error("out of memory on node record allocation");
		free(drec);
		return NULL;
	}
	memcpy(nrec, &db->nrec, sizeof(node_rec_t));

	/* update discovery record */
	drec->type = type;
	if (drec->type == DISCOVERY_TYPE_SENDTARGETS) {
		strncpy(drec->u.sendtargets.address, ip, 16);
		drec->u.sendtargets.port = port;
	} else if (drec->type == DISCOVERY_TYPE_SLP) {
		log_error("not implemented discovery type");
	} else if (drec->type == DISCOVERY_TYPE_ISNS) {
		log_error("not implemented discovery type");
	}

	/*
	 * Discovery info example:
	 *
	 * DTN=iqn.2001-04.com.example:storage.disk2.sys1.xyz
	 * TT=1
	 * TP=3260
	 * TA=10.16.16.227
	 * ;
	 * DTN=iqn.2001-04.com.example:storage.disk2.sys2.xyz
	 * TT=1
	 * TP=3260
	 * TA=10.16.16.228
	 * ;
	 * !
	 */
	ptr = newinfo = strdup(info);
	while (*ptr) {
		char *dp;

		/* convert line to zero-string */
		if ((dp = strchr(ptr, '\n'))) {
			*dp = '\0';
		}

		/* separate name and value */
		if ((dp = strchr(ptr, '='))) {
			*dp = '\0'; dp++;
			if (!strcmp(ptr, "DTN")) {
				strncpy(nrec->name, dp, TARGET_NAME_MAXLEN);
			} else if (!strcmp(ptr, "TT")) {
				nrec->tpgt = strtoul(dp, NULL, 10);
			} else if (!strcmp(ptr, "TP")) {
				nrec->cnx[0].port = strtoul(dp, NULL, 10);
			} else if (!strcmp(ptr, "TA")) {
				strncpy(nrec->cnx[0].address, dp, 16);
			} else {
				log_error("can not parse discovery info value."
					  "Bug?");
				free(drec);
				drec = NULL;
				goto out;
			}
			log_debug(7, "discovery info key %s value %s", ptr, dp);
			ptr = dp + strlen(dp) + 1;
		} else if (*ptr == ';') {
			/* end of entry */
			ptr += 2;
			if (idbm_add_discovery(db, drec)) {
				log_error("can not update discovery record.");
				free(drec);
				drec = NULL;
				goto out;
			}
			if (idbm_add_node(db, drec, nrec)) {
				log_error("can not update node record.");
				free(drec);
				drec = NULL;
				goto out;
			}
		} else if (*ptr == '!') {
			/* end of discovery info */
			ptr += 2;
		} else {
			log_error("can not parse discovery info key. Bug?");
			free(drec);
			drec = NULL;
			goto out;
		}
	}

out:
	free(nrec);
	free(newinfo);
	return drec;
}

void
idbm_sendtargets_defaults(idbm_t *db, struct iscsi_sendtargets_config *cfg)
{
	memcpy(cfg, &db->drec_st.u.sendtargets,
	       sizeof(struct iscsi_sendtargets_config));
}

void
idbm_slp_defaults(idbm_t *db, struct iscsi_slp_config *cfg)
{
	memcpy(cfg, &db->drec_slp.u.slp,
	       sizeof(struct iscsi_slp_config));
}

idbm_t*
idbm_init(char *configfile)
{
	idbm_t *db;

	db = malloc(sizeof(idbm_t));
	if (!db) {
		log_error("out of memory on idbm allocation");
		return NULL;
	}
	memset(db, 0, sizeof(idbm_t));

	db->configfile = strdup(configfile);
	idbm_sync_config(db);

	if ((db->discdb = idbm_open(DISCOVERY_FILE,
				access(DISCOVERY_FILE, F_OK) != 0 ?
					O_CREAT|O_RDWR : O_RDWR)) == NULL) {
		free(db->configfile);
		free(db);
		return NULL;
	}

	if ((db->nodedb = idbm_open(NODE_FILE, access(NODE_FILE, F_OK) != 0 ?
				O_CREAT|O_RDWR : O_RDWR)) == NULL) {
		idbm_close(db->discdb);
		free(db->configfile);
		free(db);
		return NULL;
	}

	return db;
}

void
idbm_terminate(idbm_t *db)
{
	idbm_close(db->discdb);
	idbm_close(db->nodedb);
	free(db->configfile);
	free(db);
}