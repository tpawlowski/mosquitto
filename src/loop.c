/*
Copyright (c) 2009-2013 Roger Light <roger@atchoo.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of mosquitto nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE

#include <config.h>

#include <assert.h>
#ifndef WIN32
#include <poll.h>
#else
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef WITH_WEBSOCKETS
#  include <libwebsockets.h>
#endif

#include <mosquitto_broker.h>
#include <memory_mosq.h>
#include <time_mosq.h>
#include <util_mosq.h>

extern bool flag_reload;
#ifdef WITH_PERSISTENCE
extern bool flag_db_backup;
#endif
extern bool flag_tree_print;
extern int run;
#ifdef WITH_SYS_TREE
extern int g_clients_expired;
#endif

static void loop_handle_errors(struct mosquitto_db *db, struct pollfd *pollfds);
static void loop_handle_reads_writes(struct mosquitto_db *db, struct pollfd *pollfds);

int mosquitto_main_loop(struct mosquitto_db *db, int *listensock, int listensock_count, int listener_max)
{
	time_t start_time = mosquitto_time();
	time_t last_backup = mosquitto_time();
	time_t last_store_clean = mosquitto_time();
	time_t now = 0;
	time_t now_time;
	int time_count;
	int fdcount;
	struct mosquitto *context, *ctxt_tmp;
#ifndef WIN32
	sigset_t sigblock, origsig;
#endif
	int i;
	struct pollfd *pollfds = NULL;
	int pollfd_count = 0;
	int pollfd_index;
#ifdef WITH_BRIDGE
	int bridge_sock;
	int rc;
#endif
	int context_count;
	time_t expiration_check_time = 0;

#ifndef WIN32
	sigemptyset(&sigblock);
	sigaddset(&sigblock, SIGINT);
#endif

	if(db->config->persistent_client_expiration > 0){
		expiration_check_time = time(NULL) + db->config->persistent_client_expiration;
	}

	while(run){
		HASH_ITER(hh_for_free, db->contexts_for_free, context, ctxt_tmp){
			HASH_DELETE(hh_for_free, db->contexts_for_free, context);
			mqtt3_context_cleanup(db, context, true);
		}
#ifdef WITH_SYS_TREE
		if(db->config->sys_interval > 0){
			mqtt3_db_sys_update(db, db->config->sys_interval, start_time);
		}
#endif

		context_count = HASH_CNT(hh_sock, db->contexts_by_sock);
		if(listensock_count + context_count > pollfd_count || !pollfds){
			pollfd_count = listensock_count + context_count;
			pollfds = _mosquitto_realloc(pollfds, sizeof(struct pollfd)*pollfd_count);
			if(!pollfds){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
				return MOSQ_ERR_NOMEM;
			}
		}

		memset(pollfds, -1, sizeof(struct pollfd)*pollfd_count);

		pollfd_index = 0;
		for(i=0; i<listensock_count; i++){
			pollfds[pollfd_index].fd = listensock[i];
			pollfds[pollfd_index].events = POLLIN;
			pollfds[pollfd_index].revents = 0;
			pollfd_index++;
		}

		now_time = time(NULL);

		time_count = 0;
		HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
			if(time_count > 0){
				time_count--;
			}else{
				time_count = 1000;
				now = mosquitto_time();
			}
			context->pollfd_index = -1;

			if(context->sock != INVALID_SOCKET){
#ifdef WITH_BRIDGE
				if(context->bridge){
					_mosquitto_check_keepalive(db, context);
					if(context->bridge->round_robin == false
							&& context->bridge->cur_address != 0
							&& now > context->bridge->primary_retry){

						/* FIXME - this should be non-blocking */
						if(_mosquitto_try_connect(context->bridge->addresses[0].address, context->bridge->addresses[0].port, &bridge_sock, NULL, true) == MOSQ_ERR_SUCCESS){
							COMPAT_CLOSE(bridge_sock);
							_mosquitto_socket_close(db, context);
							context->bridge->cur_address = context->bridge->address_count-1;
						}
					}
				}
#endif

				/* Local bridges never time out in this fashion. */
				if(!(context->keepalive) 
						|| context->bridge
						|| now - context->last_msg_in < (time_t)(context->keepalive)*3/2){

					if(mqtt3_db_message_write(context) == MOSQ_ERR_SUCCESS){
						pollfds[pollfd_index].fd = context->sock;
						pollfds[pollfd_index].events = POLLIN;
						pollfds[pollfd_index].revents = 0;
						if(context->current_out_packet){
							pollfds[pollfd_index].events |= POLLOUT;
						}
						context->pollfd_index = pollfd_index;
						pollfd_index++;
					}else{
						do_disconnect(db, context);
					}
				}else{
					if(db->config->connection_messages == true){
						_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s has exceeded timeout, disconnecting.", context->id);
					}
					/* Client has exceeded keepalive*1.5 */
					do_disconnect(db, context);
				}
			}
		}

#ifdef WITH_BRIDGE
		time_count = 0;
		HASH_ITER(hh_bridge, db->contexts_bridge, context, ctxt_tmp){
			if(context->sock == INVALID_SOCKET){
				if(time_count > 0){
					time_count--;
				}else{
					time_count = 1000;
					now = mosquitto_time();
				}
				/* Want to try to restart the bridge connection */
				if(!context->bridge->restart_t){
					context->bridge->restart_t = now+context->bridge->restart_timeout;
					context->bridge->cur_address++;
					if(context->bridge->cur_address == context->bridge->address_count){
						context->bridge->cur_address = 0;
					}
					if(context->bridge->round_robin == false && context->bridge->cur_address != 0){
						context->bridge->primary_retry = now + 5;
					}
				}else{
					if(context->bridge->start_type == bst_lazy && context->bridge->lazy_reconnect){
						rc = mqtt3_bridge_connect(db, context);
						if(rc){
							context->bridge->cur_address++;
							if(context->bridge->cur_address == context->bridge->address_count){
								context->bridge->cur_address = 0;
							}
						}
					}
					if(context->bridge->start_type == bst_automatic && now > context->bridge->restart_t){
						context->bridge->restart_t = 0;
						rc = mqtt3_bridge_connect(db, context);
						if(rc == MOSQ_ERR_SUCCESS){
							pollfds[pollfd_index].fd = context->sock;
							pollfds[pollfd_index].events = POLLIN;
							pollfds[pollfd_index].revents = 0;
							if(context->current_out_packet){
								pollfds[pollfd_index].events |= POLLOUT;
							}
							context->pollfd_index = pollfd_index;
							pollfd_index++;
						}else{
							/* Retry later. */
							context->bridge->restart_t = now+context->bridge->restart_timeout;

							context->bridge->cur_address++;
							if(context->bridge->cur_address == context->bridge->address_count){
								context->bridge->cur_address = 0;
							}
						}
					}
				}
			}
		}
#endif
		now_time = time(NULL);
		if(db->config->persistent_client_expiration > 0 && now_time > expiration_check_time){
			HASH_ITER(hh_id, db->contexts_by_id, context, ctxt_tmp){
				if(context->sock == -1 && context->clean_session == 0){
					/* This is a persistent client, check to see if the
					 * last time it connected was longer than
					 * persistent_client_expiration seconds ago. If so,
					 * expire it and clean up.
					 */
					if(now_time > context->disconnect_t+db->config->persistent_client_expiration){
						_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Expiring persistent client %s due to timeout.", context->id);
#ifdef WITH_SYS_TREE
						g_clients_expired++;
#endif
						context->clean_session = true;
						mqtt3_context_cleanup(db, context, true);
						context = NULL;
					}
				}
			}
			expiration_check_time = time(NULL) + db->config->persistent_client_expiration;
		}

		mqtt3_db_message_timeout_check(db, db->config->retry_interval);

#ifndef WIN32
		sigprocmask(SIG_SETMASK, &sigblock, &origsig);
		fdcount = poll(pollfds, pollfd_index, 100);
		sigprocmask(SIG_SETMASK, &origsig, NULL);
#else
		fdcount = WSAPoll(pollfds, pollfd_index, 100);
#endif
		if(fdcount == -1){
			loop_handle_errors(db, pollfds);
		}else{
			loop_handle_reads_writes(db, pollfds);

			for(i=0; i<listensock_count; i++){
				if(pollfds[i].revents & (POLLIN | POLLPRI)){
					while(mqtt3_socket_accept(db, listensock[i]) != -1){
					}
				}
			}
		}
#ifdef WITH_PERSISTENCE
		if(db->config->persistence && db->config->autosave_interval){
			if(db->config->autosave_on_changes){
				if(db->persistence_changes > db->config->autosave_interval){
					mqtt3_db_backup(db, false, false);
					db->persistence_changes = 0;
				}
			}else{
				if(last_backup + db->config->autosave_interval < mosquitto_time()){
					mqtt3_db_backup(db, false, false);
					last_backup = mosquitto_time();
				}
			}
		}
#endif
		if(!db->config->store_clean_interval || last_store_clean + db->config->store_clean_interval < mosquitto_time()){
			mqtt3_db_store_clean(db);
			last_store_clean = mosquitto_time();
		}
#ifdef WITH_PERSISTENCE
		if(flag_db_backup){
			mqtt3_db_backup(db, false, false);
			flag_db_backup = false;
		}
#endif
		if(flag_reload){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Reloading config.");
			mqtt3_config_read(db->config, true);
			mosquitto_security_cleanup(db, true);
			mosquitto_security_init(db, true);
			mosquitto_security_apply(db);
			mqtt3_log_init(db->config->log_type, db->config->log_dest);
			flag_reload = false;
		}
		if(flag_tree_print){
			mqtt3_sub_tree_print(&db->subs, 0);
			flag_tree_print = false;
		}
#ifdef WITH_WEBSOCKETS
		for(i=0; i<db->config->listener_count; i++){
			/* Extremely hacky, should be using the lws provided external poll
			 * interface, but their interface has changed recently and ours
			 * will soon, so for now websockets clients are second class
			 * citizens. */
			if(db->config->listeners[i].ws_context){
				libwebsocket_service(db->config->listeners[i].ws_context, 0);
			}
		}
#endif
	}

	if(pollfds) _mosquitto_free(pollfds);
	return MOSQ_ERR_SUCCESS;
}

void do_disconnect(struct mosquitto_db *db, struct mosquitto *context)
{
	if(context->state == mosq_cs_disconnected){
		return;
	}
#ifdef WITH_WEBSOCKETS
	if(context->wsi){
		if(context->state != mosq_cs_disconnecting){
			context->state = mosq_cs_disconnect_ws;
		}
		if(context->wsi){
			libwebsocket_callback_on_writable(context->ws_context, context->wsi);
		}
		context->sock = INVALID_SOCKET;
	}else{
#endif
		if(db->config->connection_messages == true){
			if(context->state != mosq_cs_disconnecting){
				_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Socket error on client %s, disconnecting.", context->id);
			}else{
				_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s disconnected.", context->id);
			}
		}
		mqtt3_context_disconnect(db, context);
		if(context->clean_session){
			HASH_ADD_KEYPTR(hh_for_free, db->contexts_for_free, context, sizeof(void *), context);
			if(context->id){
				HASH_DELETE(hh_id, db->contexts_by_id, context);
				_mosquitto_free(context->id);
				context->id = NULL;
			}
		}
#ifdef WITH_WEBSOCKETS
	}
#endif
	context->state = mosq_cs_disconnected;
}

/* Error ocurred, probably an fd has been closed. 
 * Loop through and check them all.
 */
static void loop_handle_errors(struct mosquitto_db *db, struct pollfd *pollfds)
{
	struct mosquitto *context, *ctxt_tmp;

	HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
		if(pollfds[context->pollfd_index].revents & (POLLERR | POLLNVAL)){
			do_disconnect(db, context);
		}
	}
}

static void loop_handle_reads_writes(struct mosquitto_db *db, struct pollfd *pollfds)
{
	struct mosquitto *context, *ctxt_tmp;

	HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
		assert(pollfds[context->pollfd_index].fd == context->sock);
#ifdef WITH_TLS
		if(pollfds[context->pollfd_index].revents & POLLOUT ||
				context->want_write ||
				(context->ssl && context->state == mosq_cs_new)){
#else
		if(pollfds[context->pollfd_index].revents & POLLOUT){
#endif
			if(_mosquitto_packet_write(context)){
				do_disconnect(db, context);
				continue;
			}
		}
	}

	HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
#ifdef WITH_TLS
		if(pollfds[context->pollfd_index].revents & POLLIN ||
				(context->ssl && context->state == mosq_cs_new)){
#else
		if(pollfds[context->pollfd_index].revents & POLLIN){
#endif
			if(_mosquitto_packet_read(db, context)){
				do_disconnect(db, context);
				continue;
			}
		}

		if(pollfds[context->pollfd_index].revents & (POLLERR | POLLNVAL)){
			do_disconnect(db, context);
			continue;
		}
	}
}

