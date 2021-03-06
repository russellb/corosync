/*
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Angus Salkeld <asalkeld@redhat.com>
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Red Hat, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <sys/uio.h>
#include <string.h>

#include <qb/qbdefs.h>
#include <qb/qblist.h>
#include <qb/qbutil.h>
#include <qb/qbloop.h>
#include <qb/qbipcs.h>

#include <corosync/swab.h>
#include <corosync/corotypes.h>
#include <corosync/corodefs.h>
#include <corosync/totem/totempg.h>
#include <corosync/engine/objdb.h>
#include <corosync/engine/config.h>
#include <corosync/engine/logsys.h>

#include "mainconfig.h"
#include "sync.h"
#include "syncv2.h"
#include "timer.h"
#include "main.h"
#include "util.h"
#include "apidef.h"
#include "service.h"

LOGSYS_DECLARE_SUBSYS ("MAIN");

static struct corosync_api_v1 *api = NULL;
static int32_t ipc_not_enough_fds_left = 0;
static int32_t ipc_fc_is_quorate; /* boolean */
static int32_t ipc_fc_totem_queue_level; /* percentage used */
static int32_t ipc_fc_sync_in_process; /* boolean */
static qb_handle_t object_connection_handle;

struct cs_ipcs_mapper {
	int32_t id;
	qb_ipcs_service_t *inst;
	char name[256];
};

struct outq_item {
	void *msg;
	size_t mlen;
	struct list_head list;
};

static struct cs_ipcs_mapper ipcs_mapper[SERVICE_HANDLER_MAXIMUM_COUNT];

static int32_t cs_ipcs_job_add(enum qb_loop_priority p,	void *data, qb_loop_job_dispatch_fn fn);
static int32_t cs_ipcs_dispatch_add(enum qb_loop_priority p, int32_t fd, int32_t events,
	void *data, qb_ipcs_dispatch_fn_t fn);
static int32_t cs_ipcs_dispatch_mod(enum qb_loop_priority p, int32_t fd, int32_t events,
	void *data, qb_ipcs_dispatch_fn_t fn);
static int32_t cs_ipcs_dispatch_del(int32_t fd);


static struct qb_ipcs_poll_handlers corosync_poll_funcs = {
	.job_add = cs_ipcs_job_add,
	.dispatch_add = cs_ipcs_dispatch_add,
	.dispatch_mod = cs_ipcs_dispatch_mod,
	.dispatch_del = cs_ipcs_dispatch_del,
};

static int32_t cs_ipcs_connection_accept (qb_ipcs_connection_t *c, uid_t euid, gid_t egid);
static void cs_ipcs_connection_created(qb_ipcs_connection_t *c);
static int32_t cs_ipcs_msg_process(qb_ipcs_connection_t *c,
		void *data, size_t size);
static int32_t cs_ipcs_connection_closed (qb_ipcs_connection_t *c);
static void cs_ipcs_connection_destroyed (qb_ipcs_connection_t *c);

static struct qb_ipcs_service_handlers corosync_service_funcs = {
	.connection_accept	= cs_ipcs_connection_accept,
	.connection_created	= cs_ipcs_connection_created,
	.msg_process		= cs_ipcs_msg_process,
	.connection_closed	= cs_ipcs_connection_closed,
	.connection_destroyed	= cs_ipcs_connection_destroyed,
};

static const char* cs_ipcs_serv_short_name(int32_t service_id)
{
	const char *name;
	switch (service_id) {
	case EVS_SERVICE:
		name = "evs";
		break;
	case CLM_SERVICE:
		name = "saClm";
		break;
	case AMF_SERVICE:
		name = "saAmf";
		break;
	case CKPT_SERVICE:
		name = "saCkpt";
		break;
	case EVT_SERVICE:
		name = "saEvt";
		break;
	case LCK_SERVICE:
		name = "saLck";
		break;
	case MSG_SERVICE:
		name = "saMsg";
		break;
	case CFG_SERVICE:
		name = "cfg";
		break;
	case CPG_SERVICE:
		name = "cpg";
		break;
	case CMAN_SERVICE:
		name = "cman";
		break;
	case PCMK_SERVICE:
		name = "pacemaker.engine";
		break;
	case CONFDB_SERVICE:
		name = "confdb";
		break;
	case QUORUM_SERVICE:
		name = "quorum";
		break;
	case PLOAD_SERVICE:
		name = "pload";
		break;
	case TMR_SERVICE:
		name = "saTmr";
		break;
	case VOTEQUORUM_SERVICE:
		name = "votequorum";
		break;
	case NTF_SERVICE:
		name = "saNtf";
		break;
	case AMF_V2_SERVICE:
		name = "saAmfV2";
		break;
	case TST_SV1_SERVICE:
		name = "tst";
		break;
	case TST_SV2_SERVICE:
		name = "tst2";
		break;
	case MON_SERVICE:
		name = "mon";
		break;
	case WD_SERVICE:
		name = "wd";
		break;
	default:
		name = NULL;
		break;
	}
	return name;
}

int32_t cs_ipcs_service_destroy(int32_t service_id)
{
	if (ipcs_mapper[service_id].inst) {
		qb_ipcs_destroy(ipcs_mapper[service_id].inst);
		ipcs_mapper[service_id].inst = NULL;
	}
	return 0;
}

static int32_t cs_ipcs_connection_accept (qb_ipcs_connection_t *c, uid_t euid, gid_t egid)
{
	struct list_head *iter;
	int32_t service = qb_ipcs_service_id_get(c);

	if (ais_service[service] == NULL ||
		ais_service_exiting[service] ||
		ipcs_mapper[service].inst == NULL) {
		return -ENOSYS;
	}

	if (ipc_not_enough_fds_left) {
		return -EMFILE;
	}

	if (euid == 0 || egid == 0) {
		return 0;
	}

	for (iter = uidgid_list_head.next; iter != &uidgid_list_head;
		iter = iter->next) {

		struct uidgid_item *ugi = qb_list_entry (iter, struct uidgid_item,
			list);

		if (euid == ugi->uid || egid == ugi->gid)
			return 0;
	}
	log_printf(LOGSYS_LEVEL_ERROR, "Denied connection attempt from %d:%d", euid, egid);

	return -EACCES;
}

static char * pid_to_name (pid_t pid, char *out_name, size_t name_len)
{
	char *name;
	char *rest;
	FILE *fp;
	char fname[32];
	char buf[256];

	snprintf (fname, 32, "/proc/%d/stat", pid);
	fp = fopen (fname, "r");
	if (!fp) {
		return NULL;
	}

	if (fgets (buf, sizeof (buf), fp) == NULL) {
		fclose (fp);
		return NULL;
	}
	fclose (fp);

	name = strrchr (buf, '(');
	if (!name) {
		return NULL;
	}

	/* move past the bracket */
	name++;

	rest = strrchr (buf, ')');

	if (rest == NULL || rest[1] != ' ') {
		return NULL;
	}

	*rest = '\0';
	/* move past the NULL and space */
	rest += 2;

	/* copy the name */
	strncpy (out_name, name, name_len);
	out_name[name_len - 1] = '\0';
	return out_name;
}


struct cs_ipcs_conn_context {
	qb_handle_t stats_handle;
	struct list_head outq_head;
	int32_t queuing;
	uint32_t queued;
	uint64_t invalid_request;
	uint64_t overload;
	uint32_t sent;
	char data[1];
};

static void cs_ipcs_connection_created(qb_ipcs_connection_t *c)
{
	int32_t service = 0;
	uint32_t zero_32 = 0;
	uint64_t zero_64 = 0;
	unsigned int key_incr_dummy;
	qb_handle_t object_handle;
	struct cs_ipcs_conn_context *context;
	char conn_name[42];
	char proc_name[32];
	struct qb_ipcs_connection_stats stats;
	int32_t size = sizeof(struct cs_ipcs_conn_context);

	log_printf(LOG_INFO, "%s() new connection", __func__);

	service = qb_ipcs_service_id_get(c);

	size += ais_service[service]->private_data_size;
	context = calloc(1, size);

	list_init(&context->outq_head);
	context->queuing = QB_FALSE;
	context->queued = 0;
	context->sent = 0;

	qb_ipcs_context_set(c, context);

	ais_service[service]->lib_init_fn(c);

	api->object_key_increment (object_connection_handle,
		"active", strlen("active"),
		&key_incr_dummy);

	qb_ipcs_connection_stats_get(c, &stats, QB_FALSE);

	if (stats.client_pid > 0) {
		if (pid_to_name (stats.client_pid, proc_name, sizeof(proc_name))) {
			snprintf (conn_name,
				sizeof(conn_name),
				"%s:%d:%p", proc_name,
				stats.client_pid, c);
		} else {
			snprintf (conn_name,
				sizeof(conn_name),
				"%d:%p",
				stats.client_pid, c);
		}
	} else {
		snprintf (conn_name,
			sizeof(conn_name),
			"%p", c);
	}

	api->object_create (object_connection_handle,
		&object_handle,
		conn_name,
		strlen (conn_name));
	context->stats_handle = object_handle;

	api->object_key_create_typed (object_handle,
		"service_id",
		&zero_32, sizeof (zero_32),
		OBJDB_VALUETYPE_UINT32);

	api->object_key_create_typed (object_handle,
		"client_pid",
		&zero_32, sizeof (zero_32),
		OBJDB_VALUETYPE_INT32);

	api->object_key_create_typed (object_handle,
		"responses",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"dispatched",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"requests",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_INT64);

	api->object_key_create_typed (object_handle,
		"send_retries",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"recv_retries",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"flow_control",
		&zero_32, sizeof (zero_32),
		OBJDB_VALUETYPE_UINT32);

	api->object_key_create_typed (object_handle,
		"flow_control_count",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"queue_size",
		&zero_32, sizeof (zero_32),
		OBJDB_VALUETYPE_UINT32);

	api->object_key_create_typed (object_handle,
		"invalid_request",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);

	api->object_key_create_typed (object_handle,
		"overload",
		&zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);
}

void cs_ipc_refcnt_inc(void *conn)
{
	qb_ipcs_connection_ref(conn);
}

void cs_ipc_refcnt_dec(void *conn)
{
	qb_ipcs_connection_unref(conn);
}

void *cs_ipcs_private_data_get(void *conn)
{
	struct cs_ipcs_conn_context *cnx;
	cnx = qb_ipcs_context_get(conn);
	return &cnx->data[0];
}

static void cs_ipcs_connection_destroyed (qb_ipcs_connection_t *c)
{
	struct cs_ipcs_conn_context *context;
	struct list_head *list, *list_next;
	struct outq_item *outq_item;

	log_printf(LOG_INFO, "%s() ", __func__);

	context = qb_ipcs_context_get(c);
	if (context) {
		for (list = context->outq_head.next;
			list != &context->outq_head; list = list_next) {

			list_next = list->next;
			outq_item = list_entry (list, struct outq_item, list);

			list_del (list);
			free (outq_item->msg);
			free (outq_item);
		}
		free(context);
	}
}

static int32_t cs_ipcs_connection_closed (qb_ipcs_connection_t *c)
{
	struct cs_ipcs_conn_context *cnx;
	unsigned int key_incr_dummy;
	int32_t res = 0;
	int32_t service = qb_ipcs_service_id_get(c);

	log_printf(LOG_INFO, "%s() ", __func__);
	res = ais_service[service]->lib_exit_fn(c);
	if (res != 0) {
		return res;
	}

	cnx = qb_ipcs_context_get(c);
	api->object_destroy (cnx->stats_handle);

	api->object_key_increment (object_connection_handle,
		"closed", strlen("closed"),
		&key_incr_dummy);
	api->object_key_decrement (object_connection_handle,
		"active", strlen("active"),
		&key_incr_dummy);

	return 0;
}

int cs_ipcs_response_iov_send (void *conn,
	const struct iovec *iov,
	unsigned int iov_len)
{
	int32_t rc = qb_ipcs_response_sendv(conn, iov, iov_len);
	if (rc >= 0) {
		return 0;
	}
	return rc;
}

int cs_ipcs_response_send(void *conn, const void *msg, size_t mlen)
{
	int32_t rc = qb_ipcs_response_send(conn, msg, mlen);
	if (rc >= 0) {
		return 0;
	}
	return rc;
}

static void outq_flush (void *data)
{
	qb_ipcs_connection_t *conn = data;
	struct list_head *list, *list_next;
	struct outq_item *outq_item;
	int32_t rc;
	struct cs_ipcs_conn_context *context = qb_ipcs_context_get(conn);

	for (list = context->outq_head.next;
		list != &context->outq_head; list = list_next) {

		list_next = list->next;
		outq_item = list_entry (list, struct outq_item, list);

		rc = qb_ipcs_event_send(conn, outq_item->msg, outq_item->mlen);
		if (rc != outq_item->mlen) {
			break;
		}
		context->sent++;
		context->queued--;

		list_del (list);
		free (outq_item->msg);
		free (outq_item);
	}
	if (list_empty (&context->outq_head)) {
		context->queuing = QB_FALSE;
		log_printf(LOGSYS_LEVEL_INFO, "Q empty, queued:%d sent:%d.",
			context->queued, context->sent);
		context->queued = 0;
		context->sent = 0;
		return;
	}
	qb_loop_job_add(cs_poll_handle_get(), QB_LOOP_HIGH, conn, outq_flush);
	if (rc < 0 && rc != -EAGAIN) {
		log_printf(LOGSYS_LEVEL_ERROR, "event_send retuned %d!", rc);
	}
}

static void msg_send_or_queue(qb_ipcs_connection_t *conn, const struct iovec *iov, uint32_t iov_len)
{
	int32_t rc = 0;
	int32_t i;
	int32_t bytes_msg = 0;
	struct outq_item *outq_item;
	char *write_buf = 0;
	struct cs_ipcs_conn_context *context = qb_ipcs_context_get(conn);

	for (i = 0; i < iov_len; i++) {
		bytes_msg += iov[i].iov_len;
	}

	if (!context->queuing) {
		assert(list_empty (&context->outq_head));
		rc = qb_ipcs_event_sendv(conn, iov, iov_len);
		if (rc == bytes_msg) {
			context->sent++;
			return;
		}
		if (rc == -EAGAIN) {
			context->queued = 0;
			context->sent = 0;
			context->queuing = QB_TRUE;
			qb_loop_job_add(cs_poll_handle_get(), QB_LOOP_HIGH, conn, outq_flush);
		} else {
			log_printf(LOGSYS_LEVEL_ERROR, "event_send retuned %d, expected %d!", rc, bytes_msg);
			return;
		}
	}
	outq_item = malloc (sizeof (struct outq_item));
	if (outq_item == NULL) {
		qb_ipcs_disconnect(conn);
		return;
	}
	outq_item->msg = malloc (bytes_msg);
	if (outq_item->msg == NULL) {
		free (outq_item);
		qb_ipcs_disconnect(conn);
		return;
	}

	write_buf = outq_item->msg;
	for (i = 0; i < iov_len; i++) {
		memcpy (write_buf, iov[i].iov_base, iov[i].iov_len);
		write_buf += iov[i].iov_len;
	}
	outq_item->mlen = bytes_msg;
	list_init (&outq_item->list);
	list_add_tail (&outq_item->list, &context->outq_head);
	context->queued++;
}

int cs_ipcs_dispatch_send(void *conn, const void *msg, size_t mlen)
{
	struct iovec iov;
	iov.iov_base = (void *)msg;
	iov.iov_len = mlen;
	msg_send_or_queue (conn, &iov, 1);
	return 0;
}

int cs_ipcs_dispatch_iov_send (void *conn,
	const struct iovec *iov,
	unsigned int iov_len)
{
	msg_send_or_queue(conn, iov, iov_len);
	return 0;
}

static int32_t cs_ipcs_msg_process(qb_ipcs_connection_t *c,
		void *data, size_t size)
{
	struct qb_ipc_response_header response;
	struct qb_ipc_request_header *request_pt = (struct qb_ipc_request_header *)data;
	int32_t service = qb_ipcs_service_id_get(c);
	int32_t send_ok = 0;
	int32_t is_async_call = QB_FALSE;
	ssize_t res = -1;
	int sending_allowed_private_data;
	struct cs_ipcs_conn_context *cnx;

	send_ok = corosync_sending_allowed (service,
			request_pt->id,
			request_pt,
			&sending_allowed_private_data);

	is_async_call = (service == CPG_SERVICE && request_pt->id == 2);

	/*
	 * This happens when the message contains some kind of invalid
	 * parameter, such as an invalid size
	 */
	if (send_ok == -EINVAL) {
		response.size = sizeof (response);
		response.id = 0;
		response.error = CS_ERR_INVALID_PARAM;

		cnx = qb_ipcs_context_get(c);
		if (cnx) {
			cnx->invalid_request++;
		}

		if (is_async_call) {
			log_printf(LOGSYS_LEVEL_INFO, "*** %s() invalid message! size:%d error:%d",
				__func__, response.size, response.error);
		} else {
			qb_ipcs_response_send (c,
				&response,
				sizeof (response));
		}
		res = -EINVAL;
	} else if (send_ok < 0) {
		cnx = qb_ipcs_context_get(c);
		if (cnx) {
			cnx->overload++;
		}
		if (!is_async_call) {
			/*
			 * Overload, tell library to retry
			 */
			response.size = sizeof (response);
			response.id = 0;
			response.error = CS_ERR_TRY_AGAIN;
			qb_ipcs_response_send (c,
				&response,
				sizeof (response));
		} else {
			log_printf(LOGSYS_LEVEL_WARNING,
				"*** %s() (%d:%d - %d) %s!",
				__func__, service, request_pt->id,
				is_async_call, strerror(-send_ok));
		}
		res = -ENOBUFS;
	}

	if (send_ok) {
		ais_service[service]->lib_engine[request_pt->id].lib_handler_fn(c, request_pt);
		res = 0;
	}
	corosync_sending_allowed_release (&sending_allowed_private_data);
	return res;
}


static int32_t cs_ipcs_job_add(enum qb_loop_priority p,	void *data, qb_loop_job_dispatch_fn fn)
{
	return qb_loop_job_add(cs_poll_handle_get(), p, data, fn);
}

static int32_t cs_ipcs_dispatch_add(enum qb_loop_priority p, int32_t fd, int32_t events,
	void *data, qb_ipcs_dispatch_fn_t fn)
{
	return qb_loop_poll_add(cs_poll_handle_get(), p, fd, events, data, fn);
}

static int32_t cs_ipcs_dispatch_mod(enum qb_loop_priority p, int32_t fd, int32_t events,
	void *data, qb_ipcs_dispatch_fn_t fn)
{
	return qb_loop_poll_mod(cs_poll_handle_get(), p, fd, events, data, fn);
}

static int32_t cs_ipcs_dispatch_del(int32_t fd)
{
	return qb_loop_poll_del(cs_poll_handle_get(), fd);
}

static void cs_ipcs_low_fds_event(int32_t not_enough, int32_t fds_available)
{
	ipc_not_enough_fds_left = not_enough;
	if (not_enough) {
		log_printf(LOGSYS_LEVEL_WARNING, "refusing new connections (fds_available:%d)\n",
			fds_available);
	} else {
		log_printf(LOGSYS_LEVEL_NOTICE, "allowing new connections (fds_available:%d)\n",
			fds_available);

	}
}

int32_t cs_ipcs_q_level_get(void)
{
	return ipc_fc_totem_queue_level;
}

static qb_loop_timer_handle ipcs_check_for_flow_control_timer;
static void cs_ipcs_check_for_flow_control(void)
{
	int32_t i;
	int32_t fc_enabled;

	for (i = 0; i < SERVICE_HANDLER_MAXIMUM_COUNT; i++) {
		if (ais_service[i] == NULL || ipcs_mapper[i].inst == NULL) {
			continue;
		}
		fc_enabled = QB_TRUE;
		if (ipc_fc_is_quorate == 1 ||
			ais_service[i]->allow_inquorate == CS_LIB_ALLOW_INQUORATE) {
			/*
			 * we are quorate
			 * now check flow control
			 */
			if (ipc_fc_totem_queue_level != TOTEM_Q_LEVEL_CRITICAL &&
				ipc_fc_sync_in_process == 0) {
				fc_enabled = QB_FALSE;
			}
		}
		if (fc_enabled) {
			qb_ipcs_request_rate_limit(ipcs_mapper[i].inst, QB_IPCS_RATE_OFF);

			qb_loop_timer_add(cs_poll_handle_get(), QB_LOOP_MED, 1*QB_TIME_NS_IN_MSEC,
			       NULL, corosync_recheck_the_q_level, &ipcs_check_for_flow_control_timer);
		} else if (ipc_fc_totem_queue_level == TOTEM_Q_LEVEL_LOW) {
			qb_ipcs_request_rate_limit(ipcs_mapper[i].inst, QB_IPCS_RATE_FAST);
		} else if (ipc_fc_totem_queue_level == TOTEM_Q_LEVEL_GOOD) {
			qb_ipcs_request_rate_limit(ipcs_mapper[i].inst, QB_IPCS_RATE_NORMAL);
		} else if (ipc_fc_totem_queue_level == TOTEM_Q_LEVEL_HIGH) {
			qb_ipcs_request_rate_limit(ipcs_mapper[i].inst, QB_IPCS_RATE_SLOW);
		}
	}
}

static void cs_ipcs_fc_quorum_changed(int quorate, void *context)
{
	ipc_fc_is_quorate = quorate;
	cs_ipcs_check_for_flow_control();
}

static void cs_ipcs_totem_queue_level_changed(enum totem_q_level level)
{
	ipc_fc_totem_queue_level = level;
	cs_ipcs_check_for_flow_control();
}

void cs_ipcs_sync_state_changed(int32_t sync_in_process)
{
	ipc_fc_sync_in_process = sync_in_process;
	cs_ipcs_check_for_flow_control();
}

void cs_ipcs_stats_update(void)
{
	int32_t i;
	struct qb_ipcs_stats srv_stats;
	struct qb_ipcs_connection_stats stats;
	qb_ipcs_connection_t *c;
	struct cs_ipcs_conn_context *cnx;

	for (i = 0; i < SERVICE_HANDLER_MAXIMUM_COUNT; i++) {
		if (ais_service[i] == NULL || ipcs_mapper[i].inst == NULL) {
			continue;
		}
		qb_ipcs_stats_get(ipcs_mapper[i].inst, &srv_stats, QB_FALSE);

		for (c = qb_ipcs_connection_first_get(ipcs_mapper[i].inst); c;
		     c = qb_ipcs_connection_next_get(ipcs_mapper[i].inst, c)) {

			cnx = qb_ipcs_context_get(c);
			if (cnx == NULL) continue;

			qb_ipcs_connection_stats_get(c, &stats, QB_FALSE);

			api->object_key_replace(cnx->stats_handle,
				"client_pid", strlen("client_pid"),
				&stats.client_pid, sizeof(uint32_t));

			api->object_key_replace(cnx->stats_handle,
				"requests", strlen("requests"),
				&stats.requests, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"responses", strlen("responses"),
				&stats.responses, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"dispatched", strlen("dispatched"),
				&stats.events, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"send_retries", strlen("send_retries"),
				&stats.send_retries, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"recv_retries", strlen("recv_retries"),
				&stats.recv_retries, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"flow_control", strlen("flow_control"),
				&stats.flow_control_state, sizeof(uint32_t));
			api->object_key_replace(cnx->stats_handle,
				"flow_control_count", strlen("flow_control_count"),
				&stats.flow_control_count, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"queue_size", strlen("queue_size"),
				&cnx->queued, sizeof(uint32_t));
			api->object_key_replace(cnx->stats_handle,
				"invalid_request", strlen("invalid_request"),
				&cnx->invalid_request, sizeof(uint64_t));
			api->object_key_replace(cnx->stats_handle,
				"overload", strlen("overload"),
				&cnx->overload, sizeof(uint64_t));
			qb_ipcs_connection_unref(c);
		}
	}
}

void cs_ipcs_service_init(struct corosync_service_engine *service)
{
	if (service->lib_engine_count == 0) {
		log_printf (LOGSYS_LEVEL_DEBUG,
			"NOT Initializing IPC on %s [%d]",
			cs_ipcs_serv_short_name(service->id),
			service->id);
		return;
	}
	ipcs_mapper[service->id].id = service->id;
	strcpy(ipcs_mapper[service->id].name, cs_ipcs_serv_short_name(service->id));
	log_printf (LOGSYS_LEVEL_DEBUG,
		"Initializing IPC on %s [%d]",
		ipcs_mapper[service->id].name,
		ipcs_mapper[service->id].id);
	ipcs_mapper[service->id].inst = qb_ipcs_create(ipcs_mapper[service->id].name,
		ipcs_mapper[service->id].id,
		QB_IPC_SHM,
		&corosync_service_funcs);
	assert(ipcs_mapper[service->id].inst);
	qb_ipcs_poll_handlers_set(ipcs_mapper[service->id].inst,
		&corosync_poll_funcs);
	qb_ipcs_run(ipcs_mapper[service->id].inst);
}

void cs_ipcs_init(void)
{
	qb_handle_t object_find_handle;
	qb_handle_t object_runtime_handle;
	uint64_t zero_64 = 0;

	api = apidef_get ();

	qb_loop_poll_low_fds_event_set(cs_poll_handle_get(), cs_ipcs_low_fds_event);

	api->quorum_register_callback (cs_ipcs_fc_quorum_changed, NULL);
	totempg_queue_level_register_callback (cs_ipcs_totem_queue_level_changed);

	api->object_find_create (OBJECT_PARENT_HANDLE,
		"runtime", strlen ("runtime"),
		&object_find_handle);

	if (api->object_find_next (object_find_handle,
			&object_runtime_handle) != 0) {
		log_printf (LOGSYS_LEVEL_ERROR,"arrg no runtime");
		return;
	}

	/* Connection objects */
	api->object_create (object_runtime_handle,
		&object_connection_handle,
		"connections", strlen ("connections"));

	api->object_key_create_typed (object_connection_handle,
		"active", &zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);
	api->object_key_create_typed (object_connection_handle,
		"closed", &zero_64, sizeof (zero_64),
		OBJDB_VALUETYPE_UINT64);
}

