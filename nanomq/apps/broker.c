//
// Copyright 2021 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#include <conf.h>
#include <hash_table.h>
#include <mqtt_db.h>
#include <nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <nng/supplemental/util/options.h>
#include <nng/supplemental/util/platform.h>
#include <protocol/mqtt/mqtt_parser.h>
#include <protocol/mqtt/nmq_mqtt.h>
#include <zmalloc.h>

#include "include/bridge.h"
#include "include/nanomq.h"
#include "include/process.h"
#include "include/pub_handler.h"
#include "include/sub_handler.h"
#include "include/unsub_handler.h"
#include "include/web_server.h"

// Parallel is the maximum number of outstanding requests we can handle.
// This is *NOT* the number of threads in use, but instead represents
// outstanding work items.  Select a small number to reduce memory size.
// (Each one of these can be thought of as a request-reply loop.)  Note
// that you will probably run into limitations on the number of open file
// descriptors if you set this too high. (If not for that limit, this could
// be set in the thousands, each context consumes a couple of KB.)
#ifndef PARALLEL
#define PARALLEL 32
#endif

enum options {
	OPT_HELP = 1,
	OPT_CONFFILE,
	OPT_BRIDGEFILE,
	OPT_AUTHFILE,
	OPT_PARALLEL,
	OPT_DAEMON,
	OPT_THREADS,
	OPT_MAX_THREADS,
	OPT_PROPERTY_SIZE,
	OPT_MSQ_LEN,
	OPT_QOS_DURATION,
	OPT_URL,
	OPT_HTTP_ENABLE,
	OPT_HTTP_PORT,
};

static nng_optspec cmd_opts[] = {
	{ .o_name = "help", .o_short = 'h', .o_val = OPT_HELP },
	{ .o_name = "conf", .o_val = OPT_CONFFILE, .o_arg = true },
	{ .o_name = "bridge", .o_val = OPT_BRIDGEFILE, .o_arg = true },
	{ .o_name = "auth", .o_val = OPT_AUTHFILE, .o_arg = true },
	{ .o_name = "daemon", .o_short = 'd', .o_val = OPT_DAEMON },
	{ .o_name    = "tq_thread",
	    .o_short = 't',
	    .o_val   = OPT_THREADS,
	    .o_arg   = true },
	{ .o_name    = "max_tq_thread",
	    .o_short = 'T',
	    .o_val   = OPT_MAX_THREADS,
	    .o_arg   = true },
	{ .o_name    = "parallel",
	    .o_short = 'n',
	    .o_val   = OPT_PARALLEL,
	    .o_arg   = true },
	{ .o_name    = "property_size",
	    .o_short = 's',
	    .o_val   = OPT_PROPERTY_SIZE,
	    .o_arg   = true },
	{ .o_name    = "msq_len",
	    .o_short = 'S',
	    .o_val   = OPT_MSQ_LEN,
	    .o_arg   = true },
	{ .o_name    = "qos_duration",
	    .o_short = 'D',
	    .o_val   = OPT_QOS_DURATION,
	    .o_arg   = true },
	{ .o_name = "url", .o_val = OPT_URL, .o_arg = true },
	{ .o_name = "http", .o_val = OPT_HTTP_ENABLE, .o_arg = true },
	{ .o_name    = "port",
	    .o_short = 'p',
	    .o_val   = OPT_HTTP_PORT,
	    .o_arg   = true },
	{ .o_name = NULL, .o_val = 0 },
};

#define ASSERT_NULL(p, fmt, ...)                          \
	if ((p) != NULL) {                                \
		fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
		exit(1);                                  \
	}

// The server keeps a list of work items, sorted by expiration time,
// so that we can use this to set the timeout to the correct value for
// use in poll.

#if (defined DEBUG) && (defined ASAN)
int keepRunning = 1;
void
intHandler(int dummy)
{
	keepRunning = 0;
	fprintf(stderr, "\nBroker exit(0).\n");
}
#endif

void
fatal(const char *func, int rv)
{
	fprintf(stderr, "%s: %s\n", func, nng_strerror(rv));
}

void
server_cb(void *arg)
{
	nano_work *work = arg;
	nng_msg *  msg;
	nng_msg *  smsg = NULL;
	int        rv;

	reason_code reason;
	uint8_t *   ptr;
	conn_param *cparam = NULL;

	struct pipe_info p_info;

	switch (work->state) {
	case INIT:
		debug_msg("INIT ^^^^ ctx%d ^^^^\n", work->ctx.id);
		if (work->proto == PROTO_MQTT_BRIDGE) {
			work->state = BRIDGE;
			nng_ctx_recv(work->bridge_ctx, work->aio);
		} else {
			work->state = RECV;
			nng_ctx_recv(work->ctx, work->aio);
		}
		break;
	case RECV:
		debug_msg("RECV  ^^^^ ctx%d ^^^^\n", work->ctx.id);
		if ((rv = nng_aio_result(work->aio)) != 0) {
			debug_msg("ERROR: RECV nng aio result error: %d", rv);
			nng_aio_wait(work->aio);
		}
		msg = nng_aio_get_msg(work->aio);
		if (msg == NULL) {
			fatal("RECV NULL MSG", rv);
		}
		work->msg    = msg;
		work->cparam = nng_msg_get_conn_param(work->msg);
		work->pid    = nng_msg_get_pipe(work->msg);

		if (nng_msg_cmd_type(msg) == CMD_DISCONNECT) {
			// Disconnect reserved for will msg.
			if (conn_param_get_will_flag(work->cparam)) {
				msg = nano_msg_composer(&msg,
				    conn_param_get_will_retain(work->cparam),
				    conn_param_get_will_qos(work->cparam),
				    (mqtt_string *) conn_param_get_will_msg(
				        work->cparam),
				    (mqtt_string *) conn_param_get_will_topic(
				        work->cparam));
				nng_msg_set_cmd_type(msg, CMD_PUBLISH);
				work->msg = msg;
				handle_pub(work, work->pipe_ct);
			} else {
				work->msg   = NULL;
				work->state = RECV;
				nng_ctx_recv(work->ctx, work->aio);
				break;
			}
		} else if (nng_msg_cmd_type(msg) == CMD_PUBLISH) {
			nng_msg_set_timestamp(msg, nng_clock());
			nng_msg_set_cmd_type(msg, CMD_PUBLISH);
			handle_pub(work, work->pipe_ct);

			conf_bridge *bridge = &(work->config->bridge);
			if (bridge->bridge_mode) {
				bool found = false;
				for (size_t i = 0; i < bridge->forwards_count;
				     i++) {
					if (topic_filter(bridge->forwards[i],
					        work->pub_packet
					            ->variable_header.publish
					            .topic_name.body)) {
						found = true;
						break;
					}
				}

				if (found) {
					smsg = bridge_publish_msg(
					    work->pub_packet->variable_header
					        .publish.topic_name.body,
					    work->pub_packet->payload_body
					        .payload,
					    work->pub_packet->payload_body
					        .payload_len,
					    work->pub_packet->fixed_header.dup,
					    work->pub_packet->fixed_header.qos,
					    work->pub_packet->fixed_header
					        .retain);
					work->state = WAIT;
					nng_aio_set_msg(
					    work->bridge_aio, smsg);
					nng_ctx_send(work->bridge_ctx,
					    work->bridge_aio);
				}
			}
		} else if (nng_msg_cmd_type(msg) == CMD_CONNACK) {
			nng_msg_set_pipe(work->msg, work->pid);

			if (work->cparam != NULL) {
				conn_param_clone(
				    work->cparam); // avoid being free
			}
			// restore clean session
			char *clientid =
			    (char *) conn_param_get_clientid(work->cparam);
			if (clientid != NULL) {
				restore_session(clientid, work->cparam,
				    work->pid.id, work->db);
			}

			// clone for sending connect event notification
			nng_msg_clone(work->msg);
			nng_aio_set_msg(work->aio, work->msg);
			nng_ctx_send(work->ctx, work->aio); // send connack

			uint8_t *header = nng_msg_header(work->msg);
			uint8_t  flag   = *(header + 3);
			smsg = nano_msg_notify_connect(work->cparam, flag);

			nng_msg_set_cmd_type(smsg, CMD_PUBLISH);
			nng_msg_free(work->msg);
			work->msg = smsg;
			handle_pub(work, work->pipe_ct);

			// Free here due to the clone before
			conn_param_free(work->cparam);

			work->state = WAIT;
			nng_aio_finish(work->aio, 0);
			break;
		} else if (nng_msg_cmd_type(msg) == CMD_DISCONNECT_EV) {
			nng_msg_set_cmd_type(work->msg, CMD_PUBLISH);
			handle_pub(work, work->pipe_ct);
			// cache session
			client_ctx *cli_ctx = NULL;
			char *      clientid =
			    (char *) conn_param_get_clientid(work->cparam);
			if (clientid != NULL &&
			    conn_param_get_clean_start(work->cparam) == 0) {
				cache_session(clientid, work->cparam,
				    work->pid.id, work->db);
			}
			// free client ctx
			if (dbhash_check_id(work->pid.id)) {
				topic_queue *tq = dbhash_get_topic_queue(work->pid.id);
				while (tq) {
					if (tq->topic) {
						cli_ctx = dbtree_delete_client(
						    work->db, tq->topic, 0,
						    work->pid.id);
					}
					del_sub_ctx(cli_ctx, tq->topic);
					tq = tq->next;
				}
				dbhash_del_topic_queue(work->pid.id);
			} else {
				debug_msg("ERROR it should not happen");
			}
			cparam       = work->cparam;
			work->cparam = NULL;
			conn_param_free(cparam);
		}
		work->state = WAIT;
		nng_aio_finish(work->aio, 0);
		// nng_aio_finish_sync(work->aio, 0);
		break;
	case WAIT:
		debug_msg("WAIT ^^^^ ctx%d ^^^^", work->ctx.id);
		if (nng_msg_cmd_type(work->msg) == CMD_PINGREQ) {
			smsg = work->msg;
			nng_msg_clear(smsg);
			ptr    = nng_msg_header(smsg);
			ptr[0] = CMD_PINGRESP;
			ptr[1] = 0x00;
			nng_msg_set_cmd_type(smsg, CMD_PINGRESP);
			work->msg = smsg;
			work->pid = nng_msg_get_pipe(work->msg);
			nng_msg_set_pipe(work->msg, work->pid);
			nng_aio_set_msg(work->aio, work->msg);
			work->msg   = NULL;
			work->state = SEND;
			nng_ctx_send(work->ctx, work->aio);
			smsg = NULL;
			nng_aio_finish(work->aio, 0);
		} else if (nng_msg_cmd_type(work->msg) == CMD_PUBREC) {
			smsg   = work->msg;
			ptr    = nng_msg_header(smsg);
			ptr[0] = 0x62;
			ptr[1] = 0x02;
			nng_msg_set_cmd_type(smsg, CMD_PUBREL);
			work->msg = smsg;
			work->pid = nng_msg_get_pipe(work->msg);
			nng_msg_set_pipe(work->msg, work->pid);
			nng_aio_set_msg(work->aio, work->msg);
			work->msg   = NULL;
			work->state = SEND;
			nng_ctx_send(work->ctx, work->aio);
			smsg = NULL;
			nng_aio_finish(work->aio, 0);
		} else if (nng_msg_cmd_type(work->msg) == CMD_SUBSCRIBE) {
			nng_msg_alloc(&smsg, 0);
			work->pid     = nng_msg_get_pipe(work->msg);
			work->sub_pkt = nng_alloc(sizeof(packet_subscribe));
			if (work->sub_pkt == NULL) {
				debug_msg("ERROR: nng_alloc");
			}
			if ((reason = decode_sub_message(work)) != SUCCESS ||
			    (reason = sub_ctx_handle(work)) != SUCCESS ||
			    (reason = encode_suback_message(smsg, work)) !=
			        SUCCESS) {
				debug_msg("ERROR: sub_handler: [%d]", reason);
				if (dbhash_check_id(work->pid.id)) {
					dbhash_del_topic_queue(work->pid.id);
				}
			} else {
				// success but check info
				debug_msg("sub_pkt:"
				          " pktid: [%d]"
				          " topicLen: [%d]"
				          " topic: [%s]",
				    work->sub_pkt->packet_id,
				    work->sub_pkt->node->it->topic_filter.len,
				    work->sub_pkt->node->it->topic_filter
				        .body);
				debug_msg("suback:"
				          " headerLen: [%ld]"
				          " bodyLen: [%ld]"
				          " type: [%x]"
				          " len:[%x]"
				          " pakcetid: [%x %x].",
				    nng_msg_header_len(smsg),
				    nng_msg_len(smsg),
				    *((uint8_t *) nng_msg_header(smsg)),
				    *((uint8_t *) nng_msg_header(smsg) + 1),
				    *((uint8_t *) nng_msg_body(smsg)),
				    *((uint8_t *) nng_msg_body(smsg) + 1));
			}
			nng_msg_free(work->msg);
			destroy_sub_pkt(work->sub_pkt,
			    conn_param_get_protover(work->cparam));
			// handle retain
			if (work->msg_ret) {
				debug_msg("retain msg [%p] size [%ld] \n",
				    work->msg_ret,
				    cvector_size(work->msg_ret));
				for (int i = 0;
				     i < cvector_size(work->msg_ret); i++) {
					nng_msg *m = work->msg_ret[i];
					nng_msg_clone(m);
					work->msg = m;
					nng_aio_set_msg(work->aio, work->msg);
					nng_msg_set_pipe(work->msg, work->pid);
					nng_ctx_send(work->ctx, work->aio);
				}
				cvector_free(work->msg_ret);
			}
			nng_msg_set_cmd_type(smsg, CMD_SUBACK);
			work->msg = smsg;
			nng_msg_set_pipe(work->msg, work->pid);
			nng_aio_set_msg(work->aio, work->msg);
			work->msg   = NULL;
			work->state = SEND;
			nng_ctx_send(work->ctx, work->aio);
			smsg = NULL;
			nng_aio_finish(work->aio, 0);
			break;
		} else if (nng_msg_cmd_type(work->msg) == CMD_UNSUBSCRIBE) {
			nng_msg_alloc(&smsg, 0);
			work->unsub_pkt =
			    nng_alloc(sizeof(packet_unsubscribe));
			work->pid = nng_msg_get_pipe(work->msg);
			if (work->unsub_pkt == NULL) {
				debug_msg("ERROR: nng_alloc");
			}
			if ((reason = decode_unsub_message(work)) != SUCCESS ||
			    (reason = unsub_ctx_handle(work)) != SUCCESS ||
			    (reason = encode_unsuback_message(smsg, work)) !=
			        SUCCESS) {
				debug_msg("ERROR: unsub_handler [%d]", reason);
			} else {
				// success but check info
				debug_msg("unsub_pkt:"
				          " pktid: [%d]"
				          " topicLen: [%d]",
				    work->unsub_pkt->packet_id,
				    work->unsub_pkt->node->it->topic_filter
				        .len);
				debug_msg("unsuback:"
				          " headerLen: [%ld]"
				          " bodyLen: [%ld]."
				          " bodyType: [%x]"
				          " len: [%x]"
				          " packetid: [%x %x].",
				    nng_msg_header_len(smsg),
				    nng_msg_len(smsg),
				    *((uint8_t *) nng_msg_header(smsg)),
				    *((uint8_t *) nng_msg_header(smsg) + 1),
				    *((uint8_t *) nng_msg_body(smsg)),
				    *((uint8_t *) nng_msg_body(smsg) + 1));
			}
			// free unsub_pkt
			destroy_unsub_ctx(work->unsub_pkt);
			nng_msg_free(work->msg);

			work->msg    = smsg;
			work->pid.id = 0;
			nng_msg_set_pipe(work->msg, work->pid);
			nng_aio_set_msg(work->aio, work->msg);
			work->msg   = NULL;
			work->state = SEND;
			nng_ctx_send(work->ctx, work->aio);
			smsg = NULL;
			nng_aio_finish(work->aio, 0);
			break;
		} else if (nng_msg_cmd_type(work->msg) == CMD_PUBLISH) {
			if ((rv = nng_aio_result(work->aio)) != 0) {
				debug_msg("WAIT nng aio result error: %d", rv);
				fatal("WAIT nng_ctx_recv/send", rv);
			}
			smsg      = work->msg; // reuse the same msg
			work->msg = NULL;

			debug_msg("total pipes: %d", work->pipe_ct->total);
			// TODO rewrite this part.
			if (work->pipe_ct->total > 0) {
				p_info = work->pipe_ct->pipe_info
				             [work->pipe_ct->current_index];
				work->pipe_ct->encode_msg(smsg, p_info.work,
				    p_info.cmd, p_info.qos, 0);
				while (work->pipe_ct->total >
				    work->pipe_ct->current_index) {
					p_info =
					    work->pipe_ct->pipe_info
					        [work->pipe_ct->current_index];
					nng_msg_clone(smsg);
					work->msg = smsg;

					nng_aio_set_prov_extra(work->aio, 0,
					    (void *) (intptr_t) p_info.qos);
					nng_aio_set_msg(work->aio, work->msg);
					work->pid.id = p_info.pipe;
					nng_msg_set_pipe(work->msg, work->pid);
					work->msg = NULL;
					work->pipe_ct->current_index++;
					nng_ctx_send(work->ctx, work->aio);
				}
				if (work->pipe_ct->total <=
				    work->pipe_ct->current_index) {
					free_pub_packet(work->pub_packet);
					free_pipes_info(
					    work->pipe_ct->pipe_info);
					init_pipe_content(work->pipe_ct);
				}
				work->state = SEND;
				nng_msg_free(smsg);
				smsg = NULL;
				nng_aio_finish(work->aio, 0);
				break;
			} else {
				if (smsg) {
					nng_msg_free(smsg);
				}
				free_pub_packet(work->pub_packet);
				free_pipes_info(work->pipe_ct->pipe_info);
				init_pipe_content(work->pipe_ct);
			}

			if (work->state != SEND) {
				if (work->msg != NULL)
					nng_msg_free(work->msg);
				work->msg = NULL;
				if (work->proto == PROTO_MQTT_BRIDGE) {
					work->state = BRIDGE;
				} else {
					work->state = RECV;
				}
				nng_ctx_recv(work->ctx, work->aio);
			}
		} else if (nng_msg_cmd_type(work->msg) == CMD_PUBACK ||
		    nng_msg_cmd_type(work->msg) == CMD_PUBREL ||
		    nng_msg_cmd_type(work->msg) == CMD_PUBCOMP) {
			nng_msg_free(work->msg);
			work->msg   = NULL;
			work->state = RECV;
			nng_ctx_recv(work->ctx, work->aio);
			break;
		} else {
			debug_msg("broker has nothing to do");
			if (work->msg != NULL)
				nng_msg_free(work->msg);
			work->msg   = NULL;
			work->state = RECV;
			nng_ctx_recv(work->ctx, work->aio);
			break;
		}
		break;
	case BRIDGE:
		if ((rv = nng_aio_result(work->aio)) != 0) {
			debug_msg("nng_recv_aio: %s", nng_strerror(rv));
			work->state = RECV;
			nng_ctx_recv(work->bridge_ctx, work->aio);
			break;
		}
		work->msg = nng_aio_get_msg(work->aio);
		msg       = work->msg;

		nng_msg_set_cmd_type(msg, nng_msg_get_type(msg));
		work->msg   = msg;
		work->state = RECV;
		nng_aio_finish(work->aio, 0);
		break;
	case SEND:
		if (NULL != smsg) {
			smsg = NULL;
		}
		if ((rv = nng_aio_result(work->aio)) != 0) {
			debug_msg("SEND nng aio result error: %d", rv);
			fatal("SEND nng_ctx_send", rv);
		}
		if (work->pipe_ct->total > 0) {
			free_pub_packet(work->pub_packet);
			free_pipes_info(work->pipe_ct->pipe_info);
			init_pipe_content(work->pipe_ct);
		}
		work->msg = NULL;
		if (work->proto == PROTO_MQTT_BRIDGE) {
			work->state = BRIDGE;
			nng_ctx_recv(work->bridge_ctx, work->aio);
		} else {
			work->state = RECV;
			nng_ctx_recv(work->ctx, work->aio);
		}
		break;
	default:
		fatal("bad state!", NNG_ESTATE);
		break;
	}
}

struct work *
alloc_work(nng_socket sock)
{
	struct work *w;
	int          rv;

	if ((w = nng_alloc(sizeof(*w))) == NULL) {
		fatal("nng_alloc", NNG_ENOMEM);
	}
	if ((rv = nng_aio_alloc(&w->aio, server_cb, w)) != 0) {
		fatal("nng_aio_alloc", rv);
	}
	if ((rv = nng_ctx_open(&w->ctx, sock)) != 0) {
		fatal("nng_ctx_open", rv);
	}
	if ((rv = nng_mtx_alloc(&w->mutex)) != 0) {
		fatal("nng_mtx_alloc", rv);
	}

	w->pipe_ct = nng_alloc(sizeof(struct pipe_content));
	init_pipe_content(w->pipe_ct);

	w->state = INIT;
	return (w);
}

nano_work *
proto_work_init(nng_socket sock, nng_socket bridge_sock, uint8_t proto,
    dbtree *db_tree, dbtree *db_tree_ret, conf *config)
{
	int        rv;
	nano_work *w;
	w         = alloc_work(sock);
	w->db     = db_tree;
	w->db_ret = db_tree_ret;
	w->proto  = proto;
	w->config = config;

	if (config->bridge.bridge_mode) {
		if ((rv = nng_ctx_open(&w->bridge_ctx, bridge_sock)) != 0) {
			fatal("nng_ctx_open", rv);
		}
		if ((rv = nng_aio_alloc(&w->bridge_aio, NULL, NULL) != 0)) {
			fatal("nng_aio_alloc", rv);
		}
	}

	return w;
}

static dbtree *db     = NULL;
static dbtree *db_ret = NULL;

dbtree *
get_broker_db(void)
{
	return db;
}

int
broker(conf *nanomq_conf)
{
	nng_socket sock;

	nng_socket bridge_sock;
	nng_pipe   pipe_id;
	int        rv;
	int        i;
	// add the num of other proto
	uint64_t    num_ctx = nanomq_conf->parallel;
	const char *url     = nanomq_conf->url;

	// init tree
	dbtree_create(&db);
	if (db == NULL) {
		debug_msg("NNL_ERROR error in db create");
	}
	dbtree_create(&db_ret);
	if (db_ret == NULL) {
		debug_msg("NNL_ERROR error in db create");
	}

	/*  Create the socket. */
	nanomq_conf->db_root = db;
	sock.id              = 0;
	sock.data            = nanomq_conf;
	rv                   = nng_nmq_tcp0_open(&sock);
	if (rv != 0) {
		fatal("nng_nmq_tcp0_open", rv);
	}

	if (nanomq_conf->bridge.bridge_mode) {
		num_ctx += nanomq_conf->bridge.parallel;
		bridge_client(&bridge_sock, &nanomq_conf->bridge);
	}

	struct work *works[num_ctx];

	for (i = 0; i < nanomq_conf->parallel; i++) {
		works[i] = proto_work_init(sock, bridge_sock,
		    PROTO_MQTT_BROKER, db, db_ret, nanomq_conf);
	}

	if (nanomq_conf->bridge.bridge_mode) {
		for (i = nanomq_conf->parallel; i < num_ctx; i++) {
			works[i] = proto_work_init(sock, bridge_sock,
			    PROTO_MQTT_BRIDGE, db, db_ret, nanomq_conf);
		}
	}

	if ((rv = nng_listen(sock, url, NULL, 0)) != 0) {
		fatal("nng_listen", rv);
	}

	// read from command line & config file
	if (nanomq_conf->websocket.enable) {
		if ((rv = nng_listen(
		         sock, nanomq_conf->websocket.url, NULL, 0)) != 0) {
			fatal("nng_listen websocket", rv);
		}
	}

	for (i = 0; i < num_ctx; i++) {
		server_cb(works[i]); // this starts them going (INIT state)
	}

#if (defined DEBUG) && (defined ASAN)
	signal(SIGINT, intHandler);
	for (;;) {
		if (keepRunning == 0) {
			exit(0);
		}
		nng_msleep(6000);
	}
#else
	for (;;) {
		nng_msleep(3600000); // neither pause() nor sleep() portable
	}
#endif
}

void
print_usage(void)
{
	printf("Usage: nanomq broker { { start | restart [--url <url>] "
	       "[--conf <path>] "
	       "[--bridge <path>] \n                     "
	       "[--auth <path>] "
	       "[-d, --daemon] "
	       "[-t, --tq_thread <num>] \n                     "
	       "[-T, -max_tq_thread <num>] [-n, "
	       "--parallel <num>]\n                     "
	       "[-D, --qos_duration <num>] [--http] "
	       "[-p, --port] } \n                     "
	       "| stop }\n\n");

	printf("Options: \n");
	printf("  --url <url>                The format of "
	       "'broker+tcp://ip_addr:host' for TCP and "
	       "'nmq+ws://ip_addr:host' for WebSocket\n");
	printf("  --conf <path>              The path of a specified nanomq "
	       "configuration file \n");
	printf("  --bridge <path>            The path of a specified bridge "
	       "configuration file \n");
	printf(
	    "  --auth <path>              The path of a specified authorize "
	    "configuration file \n");
	printf("  --http                     Enable http server (default: "
	       "disable)\n");
	printf(
	    "  -p, --port <num>           The port of http server (default: "
	    "8081)\n");
	printf(
	    "  -t, --tq_thread <num>      The number of taskq threads used, "
	    "`num` greater than 0 and less than 256\n");
	printf(
	    "  -T, --max_tq_thread <num>  The maximum number of taskq threads "
	    "used, `num` greater than 0 and less than 256\n");
	printf(
	    "  -n, --parallel <num>       The maximum number of outstanding "
	    "requests we can handle\n");
	printf("  -s, --property_size <num>  The max size for a MQTT user "
	       "property\n");
	printf("  -S, --msq_len <num>        The queue length for resending "
	       "messages\n");
	printf("  -D, --qos_duration <num>   The interval of the qos timer\n");
	printf("  -d, --daemon               Set nanomq as daemon (default: "
	       "false)\n");
}

int
status_check(pid_t *pid)
{
	char * data = NULL;
	size_t size = 0;

	int rc;
	if ((rc = nng_file_get(PID_PATH_NAME, (void *) &data, &size)) != 0) {
		nng_free(data, size);
		debug_msg(".pid file not found or unreadable\n");
		return 1;
	} else {
		if ((data) != NULL) {
			sscanf(data, "%u", pid);
			debug_msg("pid read, [%u]", *pid);
			nng_free(data, size);

			if ((kill(*pid, 0)) == 0) {
				debug_msg("there is a running NanoMQ instance "
				          ": pid [%u]",
				    *pid);
				return 0;
			}
		}
		if (!nng_file_delete(PID_PATH_NAME)) {
			debug_msg(".pid file is removed");
			return 1;
		}
		debug_msg("unexpected error");
		return -1;
	}
}

int
store_pid()
{
	int  status;
	char pid_c[10] = "";

	sprintf(pid_c, "%d", getpid());
	debug_msg("%s", pid_c);

	status = nng_file_put(PID_PATH_NAME, pid_c, sizeof(pid_c));
	return status;
}

void
active_conf(conf *nanomq_conf)
{
	// check if daemonlize
	if (nanomq_conf->daemon == true && process_daemonize()) {
		fprintf(stderr, "Error occurs, cannot daemonize\n");
		exit(EXIT_FAILURE);
	}
	// taskq and max_taskq
	if (nanomq_conf->num_taskq_thread || nanomq_conf->max_taskq_thread) {
		nng_taskq_setter(nanomq_conf->num_taskq_thread,
		    nanomq_conf->max_taskq_thread);
	}
}

int
broker_parse_opts(int argc, char **argv, conf *config)
{
	int   idx = 0;
	char *arg;
	int   val;
	int   rv;

	while ((rv = nng_opts_parse(argc, argv, cmd_opts, &val, &arg, &idx)) ==
	    0) {
		switch (val) {
		case OPT_HELP:
			print_usage();
			exit(0);
			break;
		case OPT_CONFFILE:
			ASSERT_NULL(config->conf_file,
			    "CONFIG (--conf) may be specified only once.");
			config->conf_file = nng_strdup(arg);
			break;
		case OPT_BRIDGEFILE:
			ASSERT_NULL(config->bridge_file,
			    "BRIDGE (--bridge) may be specified "
			    "only once.");
			config->bridge_file = nng_strdup(arg);
			break;
		case OPT_AUTHFILE:
			ASSERT_NULL(config->auth_file,
			    "AUTH (--auth) may be specified "
			    "only once.");
			config->auth_file = nng_strdup(arg);
			break;
		case OPT_PARALLEL:
			config->parallel = atoi(arg);
			break;
		case OPT_DAEMON:
			config->daemon = true;
			break;
		case OPT_THREADS:
			config->num_taskq_thread = atoi(arg);
			break;
		case OPT_MAX_THREADS:
			config->max_taskq_thread = atoi(arg);
			break;
		case OPT_PROPERTY_SIZE:
			config->property_size = atoi(arg);
			break;
		case OPT_MSQ_LEN:
			config->msq_len = atoi(arg);
			break;
		case OPT_QOS_DURATION:
			config->qos_duration = atoi(arg);
			break;
		case OPT_URL:
			ASSERT_NULL(config->url,
			    "URL (--url) may be specified "
			    "only once.");
			config->url = nng_strdup(arg);
			break;
		case OPT_HTTP_ENABLE:
			config->http_server.enable = true;
			break;
		case OPT_HTTP_PORT:
			config->http_server.port = atoi(arg);
			break;

		default:
			break;
		}
	}

	switch (rv) {
	case NNG_EINVAL:
		fprintf(stderr,
		    "Option %s is invalid.\nTry 'nanomq broker --help' for "
		    "more information.\n",
		    argv[idx]);
		break;
	case NNG_EAMBIGUOUS:
		fprintf(stderr,
		    "Option %s is ambiguous (specify in full).\nTry 'nanomq "
		    "broker --help' for more information.\n",
		    argv[idx]);
		break;
	case NNG_ENOARG:
		fprintf(stderr,
		    "Option %s requires argument.\nTry 'nanomq broker --help' "
		    "for more information.\n",
		    argv[idx]);
		break;
	default:
		break;
	}

	return rv == -1;
}

int
broker_start(int argc, char **argv)
{
	int   i, url, temp, rc, num_ctx = 0;
	pid_t pid              = 0;
	char *conf_path        = NULL;
	char *bridge_conf_path = NULL;
	conf *nanomq_conf;

	if (!status_check(&pid)) {
		fprintf(stderr,
		    "One NanoMQ instance is still running, a new instance "
		    "won't be started until the other one is stopped.\n");
		exit(EXIT_FAILURE);
	}

	if ((nanomq_conf = nng_zalloc(sizeof(conf))) == NULL) {
		fprintf(stderr,
		    "Cannot allocate storge for configuration, quit\n");
		exit(EXIT_FAILURE);
	}

	nanomq_conf->parallel = PARALLEL;
	conf_init(nanomq_conf);

	if (!broker_parse_opts(argc, argv, nanomq_conf)) {
		conf_fini(nanomq_conf);
		return -1;
	}

	conf_parser(nanomq_conf);
	conf_bridge_parse(nanomq_conf);

	nanomq_conf->url = nanomq_conf->url != NULL
	    ? nanomq_conf->url
	    : nng_strdup(CONF_TCP_URL_DEFAULT);

	if (nanomq_conf->websocket.enable) {
		nanomq_conf->websocket.url = nanomq_conf->websocket.url != NULL
		    ? nanomq_conf->websocket.url
		    : nng_strdup(CONF_WS_URL_DEFAULT);
	}

	print_conf(nanomq_conf);
	print_bridge_conf(&nanomq_conf->bridge);

	active_conf(nanomq_conf);

	if (nanomq_conf->http_server.enable) {
		start_rest_server(nanomq_conf);
	}

	if (store_pid()) {
		debug_msg("create \"nanomq.pid\" file failed");
	}

	rc = broker(nanomq_conf);

	if (nanomq_conf->http_server.enable) {
		stop_rest_server();
	}
	exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int
broker_stop(int argc, char **argv)
{
	pid_t pid = 0;

	if (argc != 0) {
		print_usage();
		exit(EXIT_FAILURE);
	}

	if (!(status_check(&pid))) {
		kill(pid, SIGTERM);
	} else {
		fprintf(stderr, "There is no running NanoMQ instance.\n");
		exit(EXIT_FAILURE);
	}
	fprintf(stderr, "NanoMQ stopped.\n");
	exit(EXIT_SUCCESS);
}

int
broker_restart(int argc, char **argv)
{
	pid_t pid = 0;

	if (argc < 1) {
		print_usage();
		exit(EXIT_FAILURE);
	}

	if (!(status_check(&pid))) {
		kill(pid, SIGTERM);
		while (!status_check(&pid)) {
			kill(pid, SIGKILL);
		}
		fprintf(stderr, "Previous NanoMQ instance stopped.\n");
	} else {
		fprintf(stderr, "There is no running NanoMQ instance.\n");
	}

	broker_start(argc, argv);
}

int
broker_dflt(int argc, char **argv)
{
	print_usage();
	return 0;
}
