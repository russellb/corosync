/*
 * Copyright (c) 2002-2006 MontaVista Software, Inc.
 * Copyright (c) 2006-2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
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
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
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

/**
 * \mainpage Corosync
 *
 * This is the doxygen generated developer documentation for the Corosync
 * project.  For more information about Corosync, please see the project
 * web site, <a href="http://www.corosync.org">corosync.org</a>.
 *
 * \section license License
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
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
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

#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <semaphore.h>

#include <qb/qbdefs.h>
#include <qb/qblog.h>
#include <qb/qbloop.h>
#include <qb/qbutil.h>
#include <qb/qbipcs.h>

#include <corosync/swab.h>
#include <corosync/corotypes.h>
#include <corosync/corodefs.h>
#include <corosync/list.h>
#include <corosync/lcr/lcr_ifact.h>
#include <corosync/totem/totempg.h>
#include <corosync/engine/objdb.h>
#include <corosync/engine/config.h>
#include <corosync/engine/logsys.h>

#include "quorum.h"
#include "totemsrp.h"
#include "mainconfig.h"
#include "totemconfig.h"
#include "main.h"
#include "sync.h"
#include "syncv2.h"
#include "timer.h"
#include "util.h"
#include "apidef.h"
#include "service.h"
#include "schedwrk.h"
#include "evil.h"

#ifdef HAVE_SMALL_MEMORY_FOOTPRINT
#define IPC_LOGSYS_SIZE			1024*64
#else
#define IPC_LOGSYS_SIZE			8192*128
#endif

LOGSYS_DECLARE_SYSTEM ("corosync",
	LOGSYS_MODE_OUTPUT_STDERR,
	LOG_DAEMON,
	LOG_INFO);

LOGSYS_DECLARE_SUBSYS ("MAIN");

#define SERVER_BACKLOG 5

static int sched_priority = 0;

static unsigned int service_count = 32;

static struct totem_logging_configuration totem_logging_configuration;

static int num_config_modules;

static struct config_iface_ver0 *config_modules[MAX_DYNAMIC_SERVICES];

static struct objdb_iface_ver0 *objdb = NULL;

static struct corosync_api_v1 *api = NULL;

static enum cs_sync_mode minimum_sync_mode;

static int sync_in_process = 1;

static qb_loop_t *corosync_poll_handle;

struct sched_param global_sched_param;

static hdb_handle_t object_memb_handle;

static corosync_timer_handle_t corosync_stats_timer_handle;

static const char *corosync_lock_file = LOCALSTATEDIR"/run/corosync.pid";

qb_loop_t *cs_poll_handle_get (void)
{
	return (corosync_poll_handle);
}

int cs_poll_dispatch_add (qb_loop_t * handle,
		int fd,
		int events,
		void *data,

		int (*dispatch_fn) (int fd,
			int revents,
			void *data))
{
	return qb_loop_poll_add(handle, QB_LOOP_MED, fd, events, data,
				dispatch_fn);
}

int cs_poll_dispatch_delete(qb_loop_t * handle, int fd)
{
	return qb_loop_poll_del(handle, fd);
}

void corosync_state_dump (void)
{
	int i;

	for (i = 0; i < SERVICE_HANDLER_MAXIMUM_COUNT; i++) {
		if (ais_service[i] && ais_service[i]->exec_dump_fn) {
			ais_service[i]->exec_dump_fn ();
		}
	}
}

static void unlink_all_completed (void)
{
	api->timer_delete (corosync_stats_timer_handle);
	qb_loop_stop (corosync_poll_handle);
}

void corosync_shutdown_request (void)
{
	corosync_service_unlink_all (api, unlink_all_completed);
}

static int32_t sig_diag_handler (int num, void *data)
{
	corosync_state_dump ();
	qb_log_blackbox_write_to_file(LOCALSTATEDIR "/lib/corosync/fdata");
	return 0;
}

static int32_t sig_exit_handler (int num, void *data)
{
	corosync_service_unlink_all (api, unlink_all_completed);
	return 0;
}

static void sigsegv_handler (int num)
{
	(void)signal (SIGSEGV, SIG_DFL);
	qb_log_blackbox_write_to_file(LOCALSTATEDIR "/lib/corosync/fdata");
	qb_log_fini();
	raise (SIGSEGV);
}

static void sigabrt_handler (int num)
{
	(void)signal (SIGABRT, SIG_DFL);
	qb_log_blackbox_write_to_file(LOCALSTATEDIR "/lib/corosync/fdata");
	qb_log_fini();
	raise (SIGABRT);
}

#define LOCALHOST_IP inet_addr("127.0.0.1")

static void *corosync_group_handle;

static struct totempg_group corosync_group = {
	.group		= "a",
	.group_len	= 1
};

static void serialize_lock (void)
{
}

static void serialize_unlock (void)
{
}

static void corosync_sync_completed (void)
{
	log_printf (LOGSYS_LEVEL_NOTICE,
		"Completed service synchronization, ready to provide service.\n");
	sync_in_process = 0;

	cs_ipcs_sync_state_changed(sync_in_process);
}

static int corosync_sync_callbacks_retrieve (int sync_id,
	struct sync_callbacks *callbacks)
{
	unsigned int ais_service_index;
	int res;

	for (ais_service_index = 0;
		ais_service_index < SERVICE_HANDLER_MAXIMUM_COUNT;
		ais_service_index++) {

		if (ais_service[ais_service_index] != NULL
			&& (ais_service[ais_service_index]->sync_mode == CS_SYNC_V1
				|| ais_service[ais_service_index]->sync_mode == CS_SYNC_V1_APIV2)) {
			if (ais_service_index == sync_id) {
				break;
			}
		}
	}
	/*
	 * Try to load backwards compat sync engines
	 */
	if (ais_service_index == SERVICE_HANDLER_MAXIMUM_COUNT) {
		res = evil_callbacks_load (sync_id, callbacks);
		return (res);
	}
	callbacks->name = ais_service[ais_service_index]->name;
	callbacks->sync_init_api.sync_init_v1 = ais_service[ais_service_index]->sync_init;
	callbacks->api_version = 1;
	if (ais_service[ais_service_index]->sync_mode == CS_SYNC_V1_APIV2) {
		callbacks->api_version = 2;
	}
	callbacks->sync_process = ais_service[ais_service_index]->sync_process;
	callbacks->sync_activate = ais_service[ais_service_index]->sync_activate;
	callbacks->sync_abort = ais_service[ais_service_index]->sync_abort;
	return (0);
}

static int corosync_sync_v2_callbacks_retrieve (
	int service_id,
	struct sync_callbacks *callbacks)
{
	int res;

	if (minimum_sync_mode == CS_SYNC_V2 && service_id == CLM_SERVICE && ais_service[CLM_SERVICE] == NULL) {
		res = evil_callbacks_load (service_id, callbacks);
		return (res);
	}
	if (minimum_sync_mode == CS_SYNC_V2 && service_id == EVT_SERVICE && ais_service[EVT_SERVICE] == NULL) {
		res = evil_callbacks_load (service_id, callbacks);
		return (res);
	}
	if (ais_service[service_id] == NULL) {
		return (-1);
	}
	if (minimum_sync_mode == CS_SYNC_V1 && ais_service[service_id]->sync_mode != CS_SYNC_V2) {
		return (-1);
	}

	callbacks->name = ais_service[service_id]->name;

	callbacks->api_version = 1;
	if (ais_service[service_id]->sync_mode == CS_SYNC_V1_APIV2) {
		callbacks->api_version = 2;
	}

	callbacks->sync_init_api.sync_init_v1 = ais_service[service_id]->sync_init;
	callbacks->sync_process = ais_service[service_id]->sync_process;
	callbacks->sync_activate = ais_service[service_id]->sync_activate;
	callbacks->sync_abort = ais_service[service_id]->sync_abort;
	return (0);
}

static struct memb_ring_id corosync_ring_id;

static void member_object_joined (unsigned int nodeid)
{
	hdb_handle_t object_find_handle;
	hdb_handle_t object_node_handle;
	char * nodeint_str;
	char nodeid_str[64];
	unsigned int key_incr_dummy;

	snprintf (nodeid_str, 64, "%d", nodeid);

	objdb->object_find_create (
		object_memb_handle,
		nodeid_str,
		strlen (nodeid_str),
		&object_find_handle);

	if (objdb->object_find_next (object_find_handle,
			&object_node_handle) == 0) {

		objdb->object_key_increment (object_node_handle,
			"join_count", strlen("join_count"),
			&key_incr_dummy);

		objdb->object_key_replace (object_node_handle,
			"status", strlen("status"),
			"joined", strlen("joined"));
	} else {
		nodeint_str = (char*)api->totem_ifaces_print (nodeid);
		objdb->object_create (object_memb_handle,
			&object_node_handle,
			nodeid_str, strlen (nodeid_str));

		objdb->object_key_create_typed (object_node_handle,
			"ip",
			nodeint_str, strlen(nodeint_str),
			OBJDB_VALUETYPE_STRING);
		key_incr_dummy = 1;
		objdb->object_key_create_typed (object_node_handle,
			"join_count",
			&key_incr_dummy, sizeof (key_incr_dummy),
			OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (object_node_handle,
			"status",
			"joined", strlen("joined"),
			OBJDB_VALUETYPE_STRING);
	}
}

static void member_object_left (unsigned int nodeid)
{
	hdb_handle_t object_find_handle;
	hdb_handle_t object_node_handle;
	char nodeid_str[64];

	snprintf (nodeid_str, 64, "%u", nodeid);

	objdb->object_find_create (
		object_memb_handle,
		nodeid_str,
		strlen (nodeid_str),
		&object_find_handle);

	if (objdb->object_find_next (object_find_handle,
			&object_node_handle) == 0) {

		objdb->object_key_replace (object_node_handle,
			"status", strlen("status"),
			"left", strlen("left"));
	}
}

static void confchg_fn (
	enum totem_configuration_type configuration_type,
	const unsigned int *member_list, size_t member_list_entries,
	const unsigned int *left_list, size_t left_list_entries,
	const unsigned int *joined_list, size_t joined_list_entries,
	const struct memb_ring_id *ring_id)
{
	int i;
	int abort_activate = 0;

	if (sync_in_process == 1) {
		abort_activate = 1;
	}
	sync_in_process = 1;
	cs_ipcs_sync_state_changed(sync_in_process);
	memcpy (&corosync_ring_id, ring_id, sizeof (struct memb_ring_id));

	for (i = 0; i < left_list_entries; i++) {
		member_object_left (left_list[i]);
	}
	for (i = 0; i < joined_list_entries; i++) {
		member_object_joined (joined_list[i]);
	}
	/*
	 * Call configuration change for all services
	 */
	for (i = 0; i < service_count; i++) {
		if (ais_service[i] && ais_service[i]->confchg_fn) {
			ais_service[i]->confchg_fn (configuration_type,
				member_list, member_list_entries,
				left_list, left_list_entries,
				joined_list, joined_list_entries, ring_id);
		}
	}

	if (abort_activate) {
		sync_v2_abort ();
	}
	if (minimum_sync_mode == CS_SYNC_V2 && configuration_type == TOTEM_CONFIGURATION_TRANSITIONAL) {
		sync_v2_save_transitional (member_list, member_list_entries, ring_id);
	}
	if (minimum_sync_mode == CS_SYNC_V2 && configuration_type == TOTEM_CONFIGURATION_REGULAR) {
		sync_v2_start (member_list, member_list_entries, ring_id);
	}
}

static void priv_drop (void)
{
	return; /* TODO: we are still not dropping privs */
}

static void corosync_tty_detach (void)
{
	FILE *r;

	/*
	 * Disconnect from TTY if this is not a debug run
	 */

	switch (fork ()) {
		case -1:
			corosync_exit_error (AIS_DONE_FORK);
			break;
		case 0:
			/*
			 * child which is disconnected, run this process
			 */
			break;
		default:
			exit (0);
			break;
	}

	/* Create new session */
	(void)setsid();

	/*
	 * Map stdin/out/err to /dev/null.
	 */
	r = freopen("/dev/null", "r", stdin);
	if (r == NULL) {
		corosync_exit_error (AIS_DONE_STD_TO_NULL_REDIR);
	}
	r = freopen("/dev/null", "a", stderr);
	if (r == NULL) {
		corosync_exit_error (AIS_DONE_STD_TO_NULL_REDIR);
	}
	r = freopen("/dev/null", "a", stdout);
	if (r == NULL) {
		corosync_exit_error (AIS_DONE_STD_TO_NULL_REDIR);
	}
}

static void corosync_mlockall (void)
{
#if !defined(COROSYNC_BSD) || defined(COROSYNC_FREEBSD_GE_8)
	int res;
#endif
	struct rlimit rlimit;

	rlimit.rlim_cur = RLIM_INFINITY;
	rlimit.rlim_max = RLIM_INFINITY;
#ifndef COROSYNC_SOLARIS
	setrlimit (RLIMIT_MEMLOCK, &rlimit);
#else
	setrlimit (RLIMIT_VMEM, &rlimit);
#endif

#if defined(COROSYNC_BSD) && !defined(COROSYNC_FREEBSD_GE_8)
	/* under FreeBSD < 8 a process with locked page cannot call dlopen
	 * code disabled until FreeBSD bug i386/93396 was solved
	 */
	log_printf (LOGSYS_LEVEL_WARNING, "Could not lock memory of service to avoid page faults\n");
#else
	res = mlockall (MCL_CURRENT | MCL_FUTURE);
	if (res == -1) {
		LOGSYS_PERROR (errno, LOGSYS_LEVEL_WARNING,
			"Could not lock memory of service to avoid page faults");
	};
#endif
}


static void corosync_totem_stats_updater (void *data)
{
	totempg_stats_t * stats;
	uint32_t mtt_rx_token;
	uint32_t total_mtt_rx_token;
	uint32_t avg_backlog_calc;
	uint32_t total_backlog_calc;
	uint32_t avg_token_holdtime;
	uint32_t total_token_holdtime;
	int t, prev;
	int32_t token_count;
	uint32_t firewall_enabled_or_nic_failure;

	stats = api->totem_get_stats();

	objdb->object_key_replace (stats->hdr.handle,
		"msg_reserved", strlen("msg_reserved"),
		&stats->msg_reserved, sizeof (stats->msg_reserved));
	objdb->object_key_replace (stats->hdr.handle,
		"msg_queue_avail", strlen("msg_queue_avail"),
		&stats->msg_queue_avail, sizeof (stats->msg_queue_avail));

	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"orf_token_tx", strlen("orf_token_tx"),
		&stats->mrp->srp->orf_token_tx, sizeof (stats->mrp->srp->orf_token_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"orf_token_rx", strlen("orf_token_rx"),
		&stats->mrp->srp->orf_token_rx, sizeof (stats->mrp->srp->orf_token_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_merge_detect_tx", strlen("memb_merge_detect_tx"),
		&stats->mrp->srp->memb_merge_detect_tx, sizeof (stats->mrp->srp->memb_merge_detect_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_merge_detect_rx", strlen("memb_merge_detect_rx"),
		&stats->mrp->srp->memb_merge_detect_rx, sizeof (stats->mrp->srp->memb_merge_detect_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_join_tx", strlen("memb_join_tx"),
		&stats->mrp->srp->memb_join_tx, sizeof (stats->mrp->srp->memb_join_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_join_rx", strlen("memb_join_rx"),
		&stats->mrp->srp->memb_join_rx, sizeof (stats->mrp->srp->memb_join_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"mcast_tx", strlen("mcast_tx"),
		&stats->mrp->srp->mcast_tx,	sizeof (stats->mrp->srp->mcast_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"mcast_retx", strlen("mcast_retx"),
		&stats->mrp->srp->mcast_retx, sizeof (stats->mrp->srp->mcast_retx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"mcast_rx", strlen("mcast_rx"),
		&stats->mrp->srp->mcast_rx, sizeof (stats->mrp->srp->mcast_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_commit_token_tx", strlen("memb_commit_token_tx"),
		&stats->mrp->srp->memb_commit_token_tx, sizeof (stats->mrp->srp->memb_commit_token_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"memb_commit_token_rx", strlen("memb_commit_token_rx"),
		&stats->mrp->srp->memb_commit_token_rx, sizeof (stats->mrp->srp->memb_commit_token_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"token_hold_cancel_tx", strlen("token_hold_cancel_tx"),
		&stats->mrp->srp->token_hold_cancel_tx, sizeof (stats->mrp->srp->token_hold_cancel_tx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"token_hold_cancel_rx", strlen("token_hold_cancel_rx"),
		&stats->mrp->srp->token_hold_cancel_rx, sizeof (stats->mrp->srp->token_hold_cancel_rx));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"operational_entered", strlen("operational_entered"),
		&stats->mrp->srp->operational_entered, sizeof (stats->mrp->srp->operational_entered));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"operational_token_lost", strlen("operational_token_lost"),
		&stats->mrp->srp->operational_token_lost, sizeof (stats->mrp->srp->operational_token_lost));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"gather_entered", strlen("gather_entered"),
		&stats->mrp->srp->gather_entered, sizeof (stats->mrp->srp->gather_entered));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"gather_token_lost", strlen("gather_token_lost"),
		&stats->mrp->srp->gather_token_lost, sizeof (stats->mrp->srp->gather_token_lost));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"commit_entered", strlen("commit_entered"),
		&stats->mrp->srp->commit_entered, sizeof (stats->mrp->srp->commit_entered));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"commit_token_lost", strlen("commit_token_lost"),
		&stats->mrp->srp->commit_token_lost, sizeof (stats->mrp->srp->commit_token_lost));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"recovery_entered", strlen("recovery_entered"),
		&stats->mrp->srp->recovery_entered, sizeof (stats->mrp->srp->recovery_entered));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"recovery_token_lost", strlen("recovery_token_lost"),
		&stats->mrp->srp->recovery_token_lost, sizeof (stats->mrp->srp->recovery_token_lost));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"consensus_timeouts", strlen("consensus_timeouts"),
		&stats->mrp->srp->consensus_timeouts, sizeof (stats->mrp->srp->consensus_timeouts));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"rx_msg_dropped", strlen("rx_msg_dropped"),
		&stats->mrp->srp->rx_msg_dropped, sizeof (stats->mrp->srp->rx_msg_dropped));
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"continuous_gather", strlen("continuous_gather"),
		&stats->mrp->srp->continuous_gather, sizeof (stats->mrp->srp->continuous_gather));

	firewall_enabled_or_nic_failure = (stats->mrp->srp->continuous_gather > MAX_NO_CONT_GATHER ? 1 : 0);
	objdb->object_key_replace (stats->mrp->srp->hdr.handle,
		"firewall_enabled_or_nic_failure", strlen("firewall_enabled_or_nic_failure"),
		&firewall_enabled_or_nic_failure, sizeof (firewall_enabled_or_nic_failure));

	total_mtt_rx_token = 0;
	total_token_holdtime = 0;
	total_backlog_calc = 0;
	token_count = 0;
	t = stats->mrp->srp->latest_token;
	while (1) {
		if (t == 0)
			prev = TOTEM_TOKEN_STATS_MAX - 1;
		else
			prev = t - 1;
		if (prev == stats->mrp->srp->earliest_token)
			break;
		/* if tx == 0, then dropped token (not ours) */
		if (stats->mrp->srp->token[t].tx != 0 ||
			(stats->mrp->srp->token[t].rx - stats->mrp->srp->token[prev].rx) > 0 ) {
			total_mtt_rx_token += (stats->mrp->srp->token[t].rx - stats->mrp->srp->token[prev].rx);
			total_token_holdtime += (stats->mrp->srp->token[t].tx - stats->mrp->srp->token[t].rx);
			total_backlog_calc += stats->mrp->srp->token[t].backlog_calc;
			token_count++;
		}
		t = prev;
	}
	if (token_count) {
		mtt_rx_token = (total_mtt_rx_token / token_count);
		avg_backlog_calc = (total_backlog_calc / token_count);
		avg_token_holdtime = (total_token_holdtime / token_count);
		objdb->object_key_replace (stats->mrp->srp->hdr.handle,
			"mtt_rx_token", strlen("mtt_rx_token"),
			&mtt_rx_token, sizeof (mtt_rx_token));
		objdb->object_key_replace (stats->mrp->srp->hdr.handle,
			"avg_token_workload", strlen("avg_token_workload"),
			&avg_token_holdtime, sizeof (avg_token_holdtime));
		objdb->object_key_replace (stats->mrp->srp->hdr.handle,
			"avg_backlog_calc", strlen("avg_backlog_calc"),
			&avg_backlog_calc, sizeof (avg_backlog_calc));
	}

	cs_ipcs_stats_update();

	api->timer_add_duration (1500 * MILLI_2_NANO_SECONDS, NULL,
		corosync_totem_stats_updater,
		&corosync_stats_timer_handle);
}

static void corosync_totem_stats_init (void)
{
	totempg_stats_t * stats;
	hdb_handle_t object_find_handle;
	hdb_handle_t object_runtime_handle;
	hdb_handle_t object_totem_handle;
	uint32_t zero_32 = 0;
	uint64_t zero_64 = 0;

	stats = api->totem_get_stats();

	objdb->object_find_create (
		OBJECT_PARENT_HANDLE,
		"runtime",
		strlen ("runtime"),
		&object_find_handle);

	if (objdb->object_find_next (object_find_handle,
			&object_runtime_handle) == 0) {

		objdb->object_create (object_runtime_handle,
			&object_totem_handle,
			"totem", strlen ("totem"));
		objdb->object_create (object_totem_handle,
			&stats->hdr.handle,
			"pg", strlen ("pg"));
		objdb->object_create (stats->hdr.handle,
			&stats->mrp->hdr.handle,
			"mrp", strlen ("mrp"));
		objdb->object_create (stats->mrp->hdr.handle,
			&stats->mrp->srp->hdr.handle,
			"srp", strlen ("srp"));

		objdb->object_key_create_typed (stats->hdr.handle,
			"msg_reserved", &stats->msg_reserved,
			sizeof (stats->msg_reserved), OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (stats->hdr.handle,
			"msg_queue_avail", &stats->msg_queue_avail,
			sizeof (stats->msg_queue_avail), OBJDB_VALUETYPE_UINT32);

		/* Members object */
		objdb->object_create (stats->mrp->srp->hdr.handle,
			&object_memb_handle,
			"members", strlen ("members"));

		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"orf_token_tx",	&stats->mrp->srp->orf_token_tx,
			sizeof (stats->mrp->srp->orf_token_tx),	OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"orf_token_rx", &stats->mrp->srp->orf_token_rx,
			sizeof (stats->mrp->srp->orf_token_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_merge_detect_tx", &stats->mrp->srp->memb_merge_detect_tx,
			sizeof (stats->mrp->srp->memb_merge_detect_tx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_merge_detect_rx", &stats->mrp->srp->memb_merge_detect_rx,
			sizeof (stats->mrp->srp->memb_merge_detect_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_join_tx", &stats->mrp->srp->memb_join_tx,
			sizeof (stats->mrp->srp->memb_join_tx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_join_rx", &stats->mrp->srp->memb_join_rx,
			sizeof (stats->mrp->srp->memb_join_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"mcast_tx", &stats->mrp->srp->mcast_tx,
			sizeof (stats->mrp->srp->mcast_tx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"mcast_retx", &stats->mrp->srp->mcast_retx,
			sizeof (stats->mrp->srp->mcast_retx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"mcast_rx", &stats->mrp->srp->mcast_rx,
			sizeof (stats->mrp->srp->mcast_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_commit_token_tx", &stats->mrp->srp->memb_commit_token_tx,
			sizeof (stats->mrp->srp->memb_commit_token_tx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"memb_commit_token_rx", &stats->mrp->srp->memb_commit_token_rx,
			sizeof (stats->mrp->srp->memb_commit_token_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"token_hold_cancel_tx", &stats->mrp->srp->token_hold_cancel_tx,
			sizeof (stats->mrp->srp->token_hold_cancel_tx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"token_hold_cancel_rx", &stats->mrp->srp->token_hold_cancel_rx,
			sizeof (stats->mrp->srp->token_hold_cancel_rx), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"operational_entered", &stats->mrp->srp->operational_entered,
			sizeof (stats->mrp->srp->operational_entered), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"operational_token_lost", &stats->mrp->srp->operational_token_lost,
			sizeof (stats->mrp->srp->operational_token_lost), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"gather_entered", &stats->mrp->srp->gather_entered,
			sizeof (stats->mrp->srp->gather_entered), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"gather_token_lost", &stats->mrp->srp->gather_token_lost,
			sizeof (stats->mrp->srp->gather_token_lost), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"commit_entered", &stats->mrp->srp->commit_entered,
			sizeof (stats->mrp->srp->commit_entered), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"commit_token_lost", &stats->mrp->srp->commit_token_lost,
			sizeof (stats->mrp->srp->commit_token_lost), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"recovery_entered", &stats->mrp->srp->recovery_entered,
			sizeof (stats->mrp->srp->recovery_entered), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"recovery_token_lost", &stats->mrp->srp->recovery_token_lost,
			sizeof (stats->mrp->srp->recovery_token_lost), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"consensus_timeouts", &stats->mrp->srp->consensus_timeouts,
			sizeof (stats->mrp->srp->consensus_timeouts), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"mtt_rx_token", &zero_32,
			sizeof (zero_32), OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"avg_token_workload", &zero_32,
			sizeof (zero_32), OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"avg_backlog_calc", &zero_32,
			sizeof (zero_32), OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"rx_msg_dropped", &zero_64,
			sizeof (zero_64), OBJDB_VALUETYPE_UINT64);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"continuous_gather", &zero_32,
			sizeof (zero_32), OBJDB_VALUETYPE_UINT32);
		objdb->object_key_create_typed (stats->mrp->srp->hdr.handle,
			"firewall_enabled_or_nic_failure", &zero_32,
			sizeof (zero_32), OBJDB_VALUETYPE_UINT32);

	}
	/* start stats timer */
	api->timer_add_duration (1500 * MILLI_2_NANO_SECONDS, NULL,
		corosync_totem_stats_updater,
		&corosync_stats_timer_handle);

}


static void deliver_fn (
	unsigned int nodeid,
	const void *msg,
	unsigned int msg_len,
	int endian_conversion_required)
{
	const struct qb_ipc_request_header *header;
	int32_t service;
	int32_t fn_id;
	uint32_t id;
	uint32_t key_incr_dummy;

	header = msg;
	if (endian_conversion_required) {
		id = swab32 (header->id);
	} else {
		id = header->id;
	}

	/*
	 * Call the proper executive handler
	 */
	service = id >> 16;
	fn_id = id & 0xffff;

	if (ais_service[service] == NULL && service == EVT_SERVICE) {
		evil_deliver_fn (nodeid, service, fn_id, msg,
			endian_conversion_required);
	}

	if (!ais_service[service]) {
		return;
	}
	if (fn_id >= ais_service[service]->exec_engine_count) {
		log_printf(LOGSYS_LEVEL_WARNING, "discarded unknown message %d for service %d (max id %d)",
			fn_id, service, ais_service[service]->exec_engine_count);
		return;
	}

	objdb->object_key_increment (service_stats_handle[service][fn_id],
		"rx", strlen("rx"),
		&key_incr_dummy);

	if (endian_conversion_required) {
		assert(ais_service[service]->exec_engine[fn_id].exec_endian_convert_fn != NULL);
		ais_service[service]->exec_engine[fn_id].exec_endian_convert_fn
			((void *)msg);
	}

	ais_service[service]->exec_engine[fn_id].exec_handler_fn
		(msg, nodeid);
}

void main_get_config_modules(struct config_iface_ver0 ***modules, int *num)
{
	*modules = config_modules;
	*num = num_config_modules;
}

int main_mcast (
        const struct iovec *iovec,
        unsigned int iov_len,
        unsigned int guarantee)
{
	const struct qb_ipc_request_header *req = iovec->iov_base;
	int32_t service;
	int32_t fn_id;
	uint32_t key_incr_dummy;

	service = req->id >> 16;
	fn_id = req->id & 0xffff;

	if (ais_service[service]) {
		objdb->object_key_increment (service_stats_handle[service][fn_id],
			"tx", strlen("tx"), &key_incr_dummy);
	}

	return (totempg_groups_mcast_joined (corosync_group_handle, iovec, iov_len, guarantee));
}

static qb_loop_timer_handle recheck_the_q_level_timer;
void corosync_recheck_the_q_level(void *data)
{
	totempg_check_q_level(corosync_group_handle);
	if (cs_ipcs_q_level_get() == TOTEM_Q_LEVEL_CRITICAL) {
		qb_loop_timer_add(cs_poll_handle_get(), QB_LOOP_MED, 1*QB_TIME_NS_IN_MSEC,
			NULL, corosync_recheck_the_q_level, &recheck_the_q_level_timer);
	}
}

struct sending_allowed_private_data_struct {
	int reserved_msgs;
};


int corosync_sending_allowed (
	unsigned int service,
	unsigned int id,
	const void *msg,
	void *sending_allowed_private_data)
{
	struct sending_allowed_private_data_struct *pd =
		(struct sending_allowed_private_data_struct *)sending_allowed_private_data;
	struct iovec reserve_iovec;
	struct qb_ipc_request_header *header = (struct qb_ipc_request_header *)msg;
	int sending_allowed;

	reserve_iovec.iov_base = (char *)header;
	reserve_iovec.iov_len = header->size;

	pd->reserved_msgs = totempg_groups_joined_reserve (
		corosync_group_handle,
		&reserve_iovec, 1);
	if (pd->reserved_msgs == -1) {
		return -EINVAL;
	}

	sending_allowed = QB_FALSE;
	if (corosync_quorum_is_quorate() == 1 ||
	    ais_service[service]->allow_inquorate == CS_LIB_ALLOW_INQUORATE) {
		// we are quorate
		// now check flow control
		if (ais_service[service]->lib_engine[id].flow_control == CS_LIB_FLOW_CONTROL_NOT_REQUIRED) {
			sending_allowed = QB_TRUE;
		} else if (pd->reserved_msgs && sync_in_process == 0) {
			sending_allowed = QB_TRUE;
		} else if (pd->reserved_msgs == 0) {
			return -ENOBUFS;
		} else /* (sync_in_process) */ {
			return -EINPROGRESS;
		}
	} else {
		return -EHOSTUNREACH;
	}

	return (sending_allowed);
}

void corosync_sending_allowed_release (void *sending_allowed_private_data)
{
	struct sending_allowed_private_data_struct *pd =
		(struct sending_allowed_private_data_struct *)sending_allowed_private_data;

	if (pd->reserved_msgs == -1) {
		return;
	}
	totempg_groups_joined_release (pd->reserved_msgs);
}

int message_source_is_local (const mar_message_source_t *source)
{
	int ret = 0;

	assert (source != NULL);
	if (source->nodeid == totempg_my_nodeid_get ()) {
		ret = 1;
	}
	return ret;
}

void message_source_set (
	mar_message_source_t *source,
	void *conn)
{
	assert ((source != NULL) && (conn != NULL));
	memset (source, 0, sizeof (mar_message_source_t));
	source->nodeid = totempg_my_nodeid_get ();
	source->conn = conn;
}

static void corosync_setscheduler (void)
{
#if defined(HAVE_PTHREAD_SETSCHEDPARAM) && defined(HAVE_SCHED_GET_PRIORITY_MAX) && defined(HAVE_SCHED_SETSCHEDULER)
	int res;

	sched_priority = sched_get_priority_max (SCHED_RR);
	if (sched_priority != -1) {
		global_sched_param.sched_priority = sched_priority;
		res = sched_setscheduler (0, SCHED_RR, &global_sched_param);
		if (res == -1) {
			LOGSYS_PERROR(errno, LOGSYS_LEVEL_WARNING,
				"Could not set SCHED_RR at priority %d",
				global_sched_param.sched_priority);

			global_sched_param.sched_priority = 0;
#ifdef HAVE_QB_LOG_THREAD_PRIORITY_SET
			qb_log_thread_priority_set (SCHED_OTHER, 0);
#endif
		} else {

			/*
			 * Turn on SCHED_RR in logsys system
			 */
#ifdef HAVE_QB_LOG_THREAD_PRIORITY_SET
			res = qb_log_thread_priority_set (SCHED_RR, sched_priority);
#else
			res = -1;
#endif
			if (res == -1) {
				log_printf (LOGSYS_LEVEL_ERROR,
					    "Could not set logsys thread priority."
					    " Can't continue because of priority inversions.");
				corosync_exit_error (AIS_DONE_LOGSETUP);
			}
		}
	} else {
		LOGSYS_PERROR (errno, LOGSYS_LEVEL_WARNING,
			"Could not get maximum scheduler priority");
		sched_priority = 0;
	}
#else
	log_printf(LOGSYS_LEVEL_WARNING,
		"The Platform is missing process priority setting features.  Leaving at default.");
#endif
}

static void
_logsys_log_printf(int level, int subsys,
		const char *function_name,
		const char *file_name,
		int file_line,
		const char *format,
		...) __attribute__((format(printf, 6, 7)));

static void
_logsys_log_printf(int level, int subsys,
		const char *function_name,
		const char *file_name,
		int file_line,
		const char *format, ...)
{
	va_list ap;
	char buf[QB_LOG_MAX_LEN];
	size_t len;

	va_start(ap, format);
	qb_log_from_external_source_va(function_name, file_name,
				    format, level, file_line,
				    subsys, ap);
	va_end(ap);
}

static void fplay_key_change_notify_fn (
	object_change_type_t change_type,
	hdb_handle_t parent_object_handle,
	hdb_handle_t object_handle,
	const void *object_name_pt, size_t object_name_len,
	const void *key_name_pt, size_t key_len,
	const void *key_value_pt, size_t key_value_len,
	void *priv_data_pt)
{
	if (key_len == strlen ("dump_flight_data") &&
		memcmp ("dump_flight_data", key_name_pt, key_len) == 0) {
		qb_log_blackbox_write_to_file (LOCALSTATEDIR "/lib/corosync/fdata");
	}
	if (key_len == strlen ("dump_state") &&
		memcmp ("dump_state", key_name_pt, key_len) == 0) {
		corosync_state_dump ();
	}
}

static void corosync_fplay_control_init (void)
{
	hdb_handle_t object_find_handle;
	hdb_handle_t object_runtime_handle;
	hdb_handle_t object_blackbox_handle;

	objdb->object_find_create (OBJECT_PARENT_HANDLE,
		"runtime", strlen ("runtime"),
		&object_find_handle);

	if (objdb->object_find_next (object_find_handle,
			&object_runtime_handle) != 0) {
		return;
	}

	objdb->object_create (object_runtime_handle,
		&object_blackbox_handle,
		"blackbox", strlen ("blackbox"));

	objdb->object_key_create_typed (object_blackbox_handle,
		"dump_flight_data", "no", strlen("no"),
		OBJDB_VALUETYPE_STRING);
	objdb->object_key_create_typed (object_blackbox_handle,
		"dump_state", "no", strlen("no"),
		OBJDB_VALUETYPE_STRING);

	objdb->object_track_start (object_blackbox_handle,
		OBJECT_TRACK_DEPTH_RECURSIVE,
		fplay_key_change_notify_fn,
		NULL, NULL, NULL, NULL);
}

static void main_service_ready (void)
{
	int res;

	/*
	 * This must occur after totempg is initialized because "this_ip" must be set
	 */
	res = corosync_service_defaults_link_and_init (api);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Could not initialize default services\n");
		corosync_exit_error (AIS_DONE_INIT_SERVICES);
	}
	evil_init (api);
	cs_ipcs_init();
	corosync_totem_stats_init ();
	corosync_fplay_control_init ();
	if (minimum_sync_mode == CS_SYNC_V2) {
		log_printf (LOGSYS_LEVEL_NOTICE, "Compatibility mode set to none.  Using V2 of the synchronization engine.\n");
		sync_v2_init (
			corosync_sync_v2_callbacks_retrieve,
			corosync_sync_completed);
	} else
	if (minimum_sync_mode == CS_SYNC_V1) {
		log_printf (LOGSYS_LEVEL_NOTICE, "Compatibility mode set to whitetank.  Using V1 and V2 of the synchronization engine.\n");
		sync_register (
			corosync_sync_callbacks_retrieve,
			sync_v2_memb_list_determine,
			sync_v2_memb_list_abort,
			sync_v2_start);

		sync_v2_init (
			corosync_sync_v2_callbacks_retrieve,
			corosync_sync_completed);
	}


}

static enum e_ais_done corosync_flock (const char *lockfile, pid_t pid)
{
	struct flock lock;
	enum e_ais_done err;
	char pid_s[17];
	int fd_flag;
	int lf;

	err = AIS_DONE_EXIT;

	lf = open (lockfile, O_WRONLY | O_CREAT, 0640);
	if (lf == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't create lock file.\n");
		return (AIS_DONE_AQUIRE_LOCK);
	}

retry_fcntl:
	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	if (fcntl (lf, F_SETLK, &lock) == -1) {
		switch (errno) {
		case EINTR:
			goto retry_fcntl;
			break;
		case EAGAIN:
		case EACCES:
			log_printf (LOGSYS_LEVEL_ERROR, "Another Corosync instance is already running.\n");
			err = AIS_DONE_ALREADY_RUNNING;
			goto error_close;
			break;
		default:
			log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't aquire lock. Error was %s\n",
			    strerror(errno));
			err = AIS_DONE_AQUIRE_LOCK;
			goto error_close;
			break;
		}
	}

	if (ftruncate (lf, 0) == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't truncate lock file. Error was %s\n",
		    strerror (errno));
		err = AIS_DONE_AQUIRE_LOCK;
		goto error_close_unlink;
	}

	memset (pid_s, 0, sizeof (pid_s));
	snprintf (pid_s, sizeof (pid_s) - 1, "%u\n", pid);

retry_write:
	if (write (lf, pid_s, strlen (pid_s)) != strlen (pid_s)) {
		if (errno == EINTR) {
			goto retry_write;
		} else {
			log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't write pid to lock file. "
				"Error was %s\n", strerror (errno));
			err = AIS_DONE_AQUIRE_LOCK;
			goto error_close_unlink;
		}
	}

	if ((fd_flag = fcntl (lf, F_GETFD, 0)) == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't get close-on-exec flag from lock file. "
			"Error was %s\n", strerror (errno));
		err = AIS_DONE_AQUIRE_LOCK;
		goto error_close_unlink;
	}
	fd_flag |= FD_CLOEXEC;
	if (fcntl (lf, F_SETFD, fd_flag) == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't set close-on-exec flag to lock file. "
			"Error was %s\n", strerror (errno));
		err = AIS_DONE_AQUIRE_LOCK;
		goto error_close_unlink;
	}

	return (err);

error_close_unlink:
	unlink (lockfile);
error_close:
	close (lf);

	return (err);
}

int main (int argc, char **argv, char **envp)
{
	const char *error_string;
	struct totem_config totem_config;
	hdb_handle_t objdb_handle;
	hdb_handle_t config_handle;
	unsigned int config_version = 0;
	void *objdb_p;
	struct config_iface_ver0 *config;
	void *config_p;
	const char *config_iface_init;
	char *config_iface;
	char *iface;
	char *strtok_save_pt;
	int res, ch;
	int background, setprio;
	struct stat stat_out;
	char corosync_lib_dir[PATH_MAX];
	hdb_handle_t object_runtime_handle;
	enum e_ais_done flock_err;

	/* default configuration
	 */
	background = 1;
	setprio = 0;

	while ((ch = getopt (argc, argv, "fprv")) != EOF) {

		switch (ch) {
			case 'f':
				background = 0;
				logsys_config_mode_set (NULL, LOGSYS_MODE_OUTPUT_STDERR|LOGSYS_MODE_THREADED|LOGSYS_MODE_FORK);
				break;
			case 'p':
				break;
			case 'r':
				setprio = 1;
				break;
			case 'v':
				printf ("Corosync Cluster Engine, version '%s'\n", VERSION);
				printf ("Copyright (c) 2006-2009 Red Hat, Inc.\n");
				return EXIT_SUCCESS;

				break;
			default:
				fprintf(stderr, \
					"usage:\n"\
					"        -f     : Start application in foreground.\n"\
					"        -p     : Does nothing.    \n"\
					"        -r     : Set round robin realtime scheduling \n"\
					"        -v     : Display version and SVN revision of Corosync and exit.\n");
				return EXIT_FAILURE;
		}
	}

	/*
	 * Set round robin realtime scheduling with priority 99
	 * Lock all memory to avoid page faults which may interrupt
	 * application healthchecking
	 */
	if (setprio) {
		corosync_setscheduler ();
	}

	corosync_mlockall ();

	log_printf (LOGSYS_LEVEL_NOTICE, "Corosync Cluster Engine ('%s'): started and ready to provide service.\n", VERSION);
	log_printf (LOGSYS_LEVEL_INFO, "Corosync built-in features:" PACKAGE_FEATURES "\n");

	corosync_poll_handle = qb_loop_create ();

	qb_loop_signal_add(corosync_poll_handle, QB_LOOP_LOW,
		SIGUSR2, NULL, sig_diag_handler, NULL);
	qb_loop_signal_add(corosync_poll_handle, QB_LOOP_HIGH,
		SIGINT, NULL, sig_exit_handler, NULL);
	qb_loop_signal_add(corosync_poll_handle, QB_LOOP_HIGH,
		SIGQUIT, NULL, sig_exit_handler, NULL);
	qb_loop_signal_add(corosync_poll_handle, QB_LOOP_HIGH,
		SIGTERM, NULL, sig_exit_handler, NULL);
	(void)signal (SIGSEGV, sigsegv_handler);
	(void)signal (SIGABRT, sigabrt_handler);
#if MSG_NOSIGNAL != 0
	(void)signal (SIGPIPE, SIG_IGN);
#endif

	/*
	 * Load the object database interface
	 */
	res = lcr_ifact_reference (
		&objdb_handle,
		"objdb",
		0,
		&objdb_p,
		0);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't open configuration object database component.\n");
		corosync_exit_error (AIS_DONE_OBJDB);
	}

	objdb = (struct objdb_iface_ver0 *)objdb_p;

	objdb->objdb_init ();

	/*
	 * Initialize the corosync_api_v1 definition
	 */
	apidef_init (objdb);
	api = apidef_get ();

	num_config_modules = 0;

	/*
	 * Bootstrap in the default configuration parser or use
	 * the corosync default built in parser if the configuration parser
	 * isn't overridden
	 */
	config_iface_init = getenv("COROSYNC_DEFAULT_CONFIG_IFACE");
	if (!config_iface_init) {
		config_iface_init = "corosync_parser";
	}

	/* Make a copy so we can deface it with strtok */
	if ((config_iface = strdup(config_iface_init)) == NULL) {
		log_printf (LOGSYS_LEVEL_ERROR, "exhausted virtual memory");
		corosync_exit_error (AIS_DONE_OBJDB);
	}

	iface = strtok_r(config_iface, ":", &strtok_save_pt);
	while (iface)
	{
		res = lcr_ifact_reference (
			&config_handle,
			iface,
			config_version,
			&config_p,
			0);

		config = (struct config_iface_ver0 *)config_p;
		if (res == -1) {
			log_printf (LOGSYS_LEVEL_ERROR, "Corosync Executive couldn't open configuration component '%s'\n", iface);
			corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
		}

		res = config->config_readconfig(objdb, &error_string);
		if (res == -1) {
			log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
			corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
		}
		log_printf (LOGSYS_LEVEL_NOTICE, "%s", error_string);
		config_modules[num_config_modules++] = config;

		iface = strtok_r(NULL, ":", &strtok_save_pt);
	}
	free(config_iface);

	res = corosync_main_config_read (objdb, &error_string);
	if (res == -1) {
		/*
		 * if we are here, we _must_ flush the logsys queue
		 * and try to inform that we couldn't read the config.
		 * this is a desperate attempt before certain death
		 * and there is no guarantee that we can print to stderr
		 * nor that logsys is sending the messages where we expect.
		 */
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		fprintf(stderr, "%s", error_string);
		syslog (LOGSYS_LEVEL_ERROR, "%s", error_string);
		corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
	}

	/*
	 * Make sure required directory is present
	 */
	sprintf (corosync_lib_dir, "%s/lib/corosync", LOCALSTATEDIR);
	res = stat (corosync_lib_dir, &stat_out);
	if ((res == -1) || (res == 0 && !S_ISDIR(stat_out.st_mode))) {
		log_printf (LOGSYS_LEVEL_ERROR, "Required directory not present %s.  Please create it.\n", corosync_lib_dir);
		corosync_exit_error (AIS_DONE_DIR_NOT_PRESENT);
	}

	res = totem_config_read (objdb, &totem_config, &error_string);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
	}

	res = totem_config_keyread (objdb, &totem_config, &error_string);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
	}

	res = totem_config_validate (&totem_config, &error_string);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
	}

	totem_config.totem_logging_configuration = totem_logging_configuration;
	totem_config.totem_logging_configuration.log_subsys_id = _logsys_subsys_create("TOTEM", "totem");
	totem_config.totem_logging_configuration.log_level_security = LOGSYS_LEVEL_WARNING;
	totem_config.totem_logging_configuration.log_level_error = LOGSYS_LEVEL_ERROR;
	totem_config.totem_logging_configuration.log_level_warning = LOGSYS_LEVEL_WARNING;
	totem_config.totem_logging_configuration.log_level_notice = LOGSYS_LEVEL_NOTICE;
	totem_config.totem_logging_configuration.log_level_debug = LOGSYS_LEVEL_DEBUG;
	totem_config.totem_logging_configuration.log_printf = _logsys_log_printf;
	logsys_config_apply();

	res = corosync_main_config_compatibility_read (objdb,
		&minimum_sync_mode,
		&error_string);
	if (res == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		corosync_exit_error (AIS_DONE_MAINCONFIGREAD);
	}

	/* create the main runtime object */
	objdb->object_create (OBJECT_PARENT_HANDLE,
		&object_runtime_handle,
		"runtime", strlen ("runtime"));

	/*
	 * Now we are fully initialized.
	 */
	if (background) {
		corosync_tty_detach ();
	}
	qb_log_thread_start();

	if ((flock_err = corosync_flock (corosync_lock_file, getpid ())) != AIS_DONE_EXIT) {
		corosync_exit_error (flock_err);
	}

	/*
	 * if totempg_initialize doesn't have root priveleges, it cannot
	 * bind to a specific interface.  This only matters if
	 * there is more then one interface in a system, so
	 * in this case, only a warning is printed
	 */
	/*
	 * Join multicast group and setup delivery
	 *  and configuration change functions
	 */
	totempg_initialize (
		corosync_poll_handle,
		&totem_config);

	totempg_service_ready_register (
		main_service_ready);

	totempg_groups_initialize (
		&corosync_group_handle,
		deliver_fn,
		confchg_fn);

	totempg_groups_join (
		corosync_group_handle,
		&corosync_group,
		1);

	/*
	 * Drop root privleges to user 'ais'
	 * TODO: Don't really need full root capabilities;
	 *       needed capabilities are:
	 * CAP_NET_RAW (bindtodevice)
	 * CAP_SYS_NICE (setscheduler)
	 * CAP_IPC_LOCK (mlockall)
	 */
	priv_drop ();

	schedwrk_init (
		serialize_lock,
		serialize_unlock);

	/*
	 * Start main processing loop
	 */
	qb_loop_run (corosync_poll_handle);

	/*
	 * Exit was requested
	 */
	totempg_finalize ();

	/*
	 * Remove pid lock file
	 */
	unlink (corosync_lock_file);

	corosync_exit_error (AIS_DONE_EXIT);

	return EXIT_SUCCESS;
}
