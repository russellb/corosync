/*
 * Copyright (c) 2005 MontaVista Software, Inc.
 * Copyright (c) 2009 Red Hat, Inc.
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
#ifndef IPC_CFG_H_DEFINED
#define IPC_CFG_H_DEFINED

#include <netinet/in.h>
#include <corosync/corotypes.h>
#include <corosync/mar_gen.h>

enum req_lib_cfg_types {
	MESSAGE_REQ_CFG_RINGSTATUSGET = 0,
	MESSAGE_REQ_CFG_RINGREENABLE = 1,
        MESSAGE_REQ_CFG_STATETRACKSTART = 2,
        MESSAGE_REQ_CFG_STATETRACKSTOP = 3,
        MESSAGE_REQ_CFG_ADMINISTRATIVESTATESET = 4,
        MESSAGE_REQ_CFG_ADMINISTRATIVESTATEGET = 5,
        MESSAGE_REQ_CFG_SERVICELOAD = 6,
        MESSAGE_REQ_CFG_SERVICEUNLOAD = 7,
	MESSAGE_REQ_CFG_KILLNODE = 8,
	MESSAGE_REQ_CFG_TRYSHUTDOWN = 9,
	MESSAGE_REQ_CFG_REPLYTOSHUTDOWN = 10,
	MESSAGE_REQ_CFG_GET_NODE_ADDRS = 11,
	MESSAGE_REQ_CFG_LOCAL_GET = 12,
	MESSAGE_REQ_CFG_CRYPTO_SET = 13
};

enum res_lib_cfg_types {
        MESSAGE_RES_CFG_RINGSTATUSGET = 0,
        MESSAGE_RES_CFG_RINGREENABLE = 1,
        MESSAGE_RES_CFG_STATETRACKSTART = 2,
        MESSAGE_RES_CFG_STATETRACKSTOP = 3,
        MESSAGE_RES_CFG_ADMINISTRATIVESTATESET = 4,
        MESSAGE_RES_CFG_ADMINISTRATIVESTATEGET = 5,
        MESSAGE_RES_CFG_SERVICELOAD = 6,
        MESSAGE_RES_CFG_SERVICEUNLOAD = 7,
	MESSAGE_RES_CFG_KILLNODE = 8,
	MESSAGE_RES_CFG_TRYSHUTDOWN = 9,
	MESSAGE_RES_CFG_TESTSHUTDOWN = 10,
	MESSAGE_RES_CFG_GET_NODE_ADDRS = 11,
	MESSAGE_RES_CFG_LOCAL_GET = 12,
	MESSAGE_RES_CFG_REPLYTOSHUTDOWN = 13,
	MESSAGE_RES_CFG_CRYPTO_SET = 14,
};

struct req_lib_cfg_statetrack {
	struct qb_ipc_request_header header;
	uint8_t track_flags;
	corosync_cfg_state_notification_t *notification_buffer_address;
};

struct res_lib_cfg_statetrack {
	struct qb_ipc_response_header header;
};

struct req_lib_cfg_statetrackstop {
	struct qb_ipc_request_header header;
};

struct res_lib_cfg_statetrackstop {
	struct qb_ipc_response_header header;
};

struct req_lib_cfg_administrativestateset {
	struct qb_ipc_request_header header;
	cs_name_t comp_name;
	corosync_cfg_administrative_target_t administrative_target;
	corosync_cfg_administrative_state_t administrative_state;
};

struct res_lib_cfg_administrativestateset {
	struct qb_ipc_response_header header;
};

struct req_lib_cfg_administrativestateget {
	struct qb_ipc_request_header header;
	cs_name_t comp_name;
	corosync_cfg_administrative_target_t administrative_target;
	corosync_cfg_administrative_state_t administrative_state;
};

struct res_lib_cfg_administrativestateget {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_ringstatusget {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
};

struct res_lib_cfg_ringstatusget {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	mar_uint32_t interface_count __attribute__((aligned(8)));
	char interface_name[16][128] __attribute__((aligned(8)));
	char interface_status[16][512] __attribute__((aligned(8)));
};

struct req_lib_cfg_ringreenable {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
};

struct res_lib_cfg_ringreenable {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_serviceload {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	char service_name[256] __attribute__((aligned(8)));
	unsigned int service_ver;
};

struct res_lib_cfg_serviceload {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_serviceunload {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	char service_name[256] __attribute__((aligned(8)));
	unsigned int service_ver;
};

struct res_lib_cfg_serviceunload {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_killnode {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int nodeid __attribute__((aligned(8)));
	cs_name_t reason __attribute__((aligned(8)));
};

struct res_lib_cfg_killnode {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_tryshutdown {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int flags;
};

struct res_lib_cfg_tryshutdown {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct req_lib_cfg_replytoshutdown {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int response;
};

struct res_lib_cfg_replytoshutdown {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

struct res_lib_cfg_testshutdown {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	unsigned int flags;
};

struct req_lib_cfg_get_node_addrs {
        struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int nodeid;
};

struct res_lib_cfg_get_node_addrs {
        struct qb_ipc_response_header header __attribute__((aligned(8)));
	unsigned int family;
	unsigned int num_addrs;
	char addrs[TOTEMIP_ADDRLEN][0];
};

struct req_lib_cfg_local_get {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
};

struct res_lib_cfg_local_get {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	mar_uint32_t local_nodeid __attribute__((aligned(8)));
};

struct req_lib_cfg_crypto_set {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
	mar_uint32_t type __attribute__((aligned(8)));
};

struct res_lib_cfg_crypto_set {
	struct qb_ipc_response_header header __attribute__((aligned(8)));
};

typedef enum {
	AIS_AMF_ADMINISTRATIVETARGET_SERVICEUNIT = 0,
	AIS_AMF_ADMINISTRATIVETARGET_SERVICEGROUP = 1,
	AIS_AMF_ADMINISTRATIVETARGET_COMPONENTSERVICEINSTANCE = 2,
	AIS_AMF_ADMINISTRATIVETARGET_NODE = 3
} corosync_administrative_target_t;

typedef enum {
	AIS_AMF_ADMINISTRATIVESTATE_UNLOCKED = 0,
	AIS_AMF_ADMINISTRATIVESTATE_LOCKED = 1,
	AIS_AMF_ADMINISTRATIVESTATE_STOPPING = 2
} corosync_administrative_state_t;

typedef enum {
	CFG_SHUTDOWN_FLAG_REQUEST = 0,
	CFG_SHUTDOWN_FLAG_REGARDLESS = 1,
	CFG_SHUTDOWN_FLAG_IMMEDIATE = 2,
} corosync_shutdown_flags_t;


#endif /* IPC_CFG_H_DEFINED */
