
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void server_listen_cb(struct ev_loop *loop, ev_io *w, int revents);

static server_socket* server_socket_new(int fd) {
	server_socket *sock = g_slice_new0(server_socket);

	sock->refcount = 1;
	sock->watcher.data = sock;
	sock->local_addr = sockaddr_local_from_socket(fd);
	sock->local_addr_str = g_string_sized_new(0);
	sockaddr_to_string(sock->local_addr, sock->local_addr_str, FALSE);
	fd_init(fd);
	ev_init(&sock->watcher, server_listen_cb);
	ev_io_set(&sock->watcher, fd, EV_READ);
	return sock;
}

void server_socket_release(server_socket* sock) {
	if (!sock) return;
	assert(g_atomic_int_get(&sock->refcount) > 0);
	if (g_atomic_int_dec_and_test(&sock->refcount)) {
		sockaddr_clear(&sock->local_addr);
		g_string_free(sock->local_addr_str, TRUE);
		g_slice_free(server_socket, sock);
	}
}

void server_socket_acquire(server_socket* sock) {
	assert(g_atomic_int_get(&sock->refcount) > 0);
	g_atomic_int_inc(&sock->refcount);
}

static void server_value_free(gpointer _so) {
	g_slice_free(server_option, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(server_action, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(server_setup, _ss);
}

#define CATCH_SIGNAL(loop, cb, n) do {\
	ev_init(&srv->sig_w_##n, cb); \
	ev_signal_set(&srv->sig_w_##n, SIG##n); \
	ev_signal_start(loop, &srv->sig_w_##n); \
	srv->sig_w_##n.data = srv; \
	ev_unref(loop); /* Signal watchers shouldn't keep loop alive */ \
} while (0)

#define UNCATCH_SIGNAL(loop, n) do {\
	ev_ref(loop); \
	ev_signal_stop(loop, &srv->sig_w_##n); \
} while (0)

static void sigint_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	server *srv = (server*) w->data;
	UNUSED(revents);

	if (g_atomic_int_get(&srv->state) != SERVER_STOPPING) {
		INFO(srv, "%s", "Got signal, shutdown");
		server_stop(srv);
	} else {
		INFO(srv, "%s", "Got second signal, force shutdown");

		/* reset default behaviour which will kill us the third time */
		UNCATCH_SIGNAL(loop, INT);
		UNCATCH_SIGNAL(loop, TERM);
		UNCATCH_SIGNAL(loop, PIPE);
	}
}

static void sigpipe_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	/* ignore */
	UNUSED(loop); UNUSED(w); UNUSED(revents);
}

server* server_new(const gchar *module_dir) {
	server* srv = g_slice_new0(server);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = SERVER_STARTING;

	srv->workers = g_array_new(FALSE, TRUE, sizeof(worker*));

	srv->sockets = g_ptr_array_new();

	srv->modules = modules_init(srv, module_dir);

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_value_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->plugins_handle_close = g_array_new(FALSE, TRUE, sizeof(plugin*));
	srv->plugins_handle_vrclose = g_array_new(FALSE, TRUE, sizeof(plugin*));
	srv->option_def_values = g_array_new(FALSE, TRUE, sizeof(option_value));

	srv->mainaction = NULL;

	srv->exiting = FALSE;

	srv->ts_formats = g_array_new(FALSE, TRUE, sizeof(GString*));
	/* error log ts format */
	server_ts_format_add(srv, g_string_new("%a, %d %b %Y %H:%M:%S GMT"));

	log_init(srv);

	srv->io_timeout = 300; /* default I/O timeout */

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;

	server_stop(srv);
	g_atomic_int_set(&srv->exiting, TRUE);

	/* join all workers */
	{
		guint i;
		for (i = 1; i < srv->workers->len; i++) {
			worker *wrk;
			wrk = g_array_index(srv->workers, worker*, i);
			worker_exit(srv->main_worker, wrk);
			g_thread_join(wrk->thread);
		}
	}

	action_release(srv, srv->mainaction);

	/* free all workers */
	{
		guint i;
		for (i = 0; i < srv->workers->len; i++) {
			worker *wrk;
			struct ev_loop *loop;
			wrk = g_array_index(srv->workers, worker*, i);
			loop = wrk->loop;
			worker_free(wrk);
			if (i == 0) {
				ev_default_destroy();
			} else {
				ev_loop_destroy(loop);
			}
		}
		g_array_free(srv->workers, TRUE);
	}

	/* release modules */
	modules_cleanup(srv->modules);

	plugin_free(srv, srv->core_plugin);

	log_cleanup(srv);

	{
		guint i; for (i = 0; i < srv->sockets->len; i++) {
			server_socket *sock = g_ptr_array_index(srv->sockets, i);
			close(sock->watcher.fd);
			server_socket_release(sock);
		}
		g_ptr_array_free(srv->sockets, TRUE);
	}

	{
		guint i;
		for (i = 0; i < srv->ts_formats->len; i++)
			g_string_free(g_array_index(srv->ts_formats, GString*, i), TRUE);
		g_array_free(srv->ts_formats, TRUE);
	}

	g_array_free(srv->option_def_values, TRUE);
	server_plugins_free(srv);
	g_array_free(srv->plugins_handle_close, TRUE);
	g_array_free(srv->plugins_handle_vrclose, TRUE);

	if (srv->started_str)
		g_string_free(srv->started_str, TRUE);

	g_slice_free(server, srv);
}

static gpointer server_worker_cb(gpointer data) {
	worker *wrk = (worker*) data;
	worker_run(wrk);
	return NULL;
}

gboolean server_loop_init(server *srv) {
	guint i;
	struct ev_loop *loop = ev_default_loop(srv->loop_flags);

	if (!loop) {
		fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
		return FALSE;
	}

	CATCH_SIGNAL(loop, sigint_cb, INT);
	CATCH_SIGNAL(loop, sigint_cb, TERM);
	CATCH_SIGNAL(loop, sigpipe_cb, PIPE);

	if (srv->worker_count < 1) srv->worker_count = 1;
	g_array_set_size(srv->workers, srv->worker_count);
	srv->main_worker = g_array_index(srv->workers, worker*, 0) = worker_new(srv, loop);
	srv->main_worker->ndx = 0;
	for (i = 1; i < srv->worker_count; i++) {
		GError *error = NULL;
		worker *wrk;
		if (NULL == (loop = ev_loop_new(srv->loop_flags))) {
			fatal ("could not create extra libev loops");
			return FALSE;
		}
		wrk = g_array_index(srv->workers, worker*, i) = worker_new(srv, loop);
		wrk->ndx = i;
		if (NULL == (wrk->thread = g_thread_create(server_worker_cb, wrk, TRUE, &error))) {
			g_error ( "g_thread_create failed: %s", error->message );
			return FALSE;
		}
	}

	return TRUE;
}

static void server_listen_cb(struct ev_loop *loop, ev_io *w, int revents) {
	server_socket *sock = (server_socket*) w->data;
	server *srv = sock->srv;
	int s;
	sockaddr_t remote_addr;
	struct sockaddr sa;
	socklen_t l = sizeof(sa);
	UNUSED(loop);
	UNUSED(revents);

	while (-1 != (s = accept(w->fd, &sa, &l))) {
		worker *wrk = srv->main_worker;
		guint i, min_load = g_atomic_int_get(&wrk->connection_load), sel = 0;

		if (l <= sizeof(sa)) {
			remote_addr.addr = g_slice_alloc(l);
			remote_addr.len = l;
			memcpy(remote_addr.addr, &sa, l);
		} else {
			remote_addr = sockaddr_remote_from_socket(s);
		}
		l = sizeof(sa); /* reset l */

		fd_init(s);

		for (i = 1; i < srv->worker_count; i++) {
			worker *wt = g_array_index(srv->workers, worker*, i);
			guint load = g_atomic_int_get(&wt->connection_load);
			if (load < min_load) {
				wrk = wt;
				min_load = load;
				sel = i;
			}
		}

		g_atomic_int_inc((gint*) &wrk->connection_load);
		/* TRACE(srv, "selected worker %u with load %u", sel, min_load); */
		server_socket_acquire(sock);
		worker_new_con(srv->main_worker, wrk, remote_addr, s, sock);
	}

#ifdef _WIN32
	errno = WSAGetLastError();
#endif

	switch (errno) {
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case EINTR:
		/* we were stopped _before_ we had a connection */
	case ECONNABORTED: /* this is a FreeBSD thingy */
		/* we were stopped _after_ we had a connection */
		break;

	case EMFILE: /* we are out of FDs */
		server_out_of_fds(srv);
		/* TODO: disable accept callbacks? */
		break;
	default:
		ERROR(srv, "accept failed on fd=%d with error: %s", w->fd, g_strerror(errno));
		break;
	}
}

void server_listen(server *srv, int fd) {
	server_socket *sock = server_socket_new(fd);

	sock->srv = srv;
	if (g_atomic_int_get(&srv->state) == SERVER_RUNNING) ev_io_start(srv->main_worker->loop, &sock->watcher);

	g_ptr_array_add(srv->sockets, sock);
}

void server_start(server *srv) {
	guint i;
	server_state srvstate = g_atomic_int_get(&srv->state);
	if (srvstate == SERVER_STOPPING || srvstate == SERVER_RUNNING) return; /* no restart after stop */
	g_atomic_int_set(&srv->state, SERVER_RUNNING);

	if (!srv->mainaction) {
		ERROR(srv, "%s", "No action handlers defined");
		server_stop(srv);
		return;
	}

	srv->keep_alive_queue_timeout = 5;

	plugins_prepare_callbacks(srv);

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_ptr_array_index(srv->sockets, i);
		ev_io_start(srv->main_worker->loop, &sock->watcher);
	}

	srv->started = ev_now(srv->main_worker->loop);
	{
		GString *str = worker_current_timestamp(srv->main_worker, 0);
		srv->started = ev_now(srv->main_worker->loop);
		srv->started_str = g_string_new_len(GSTR_LEN(str));
	}

	log_thread_start(srv);

	worker_run(srv->main_worker);
}

void server_stop(server *srv) {
	guint i;

	if (g_atomic_int_get(&srv->state) == SERVER_STOPPING) return;
	g_atomic_int_set(&srv->state, SERVER_STOPPING);

	if (srv->main_worker) {
		for (i = 0; i < srv->sockets->len; i++) {
			server_socket *sock = g_ptr_array_index(srv->sockets, i);
			ev_io_stop(srv->main_worker->loop, &sock->watcher);
		}

		/* stop all workers */
		for (i = 0; i < srv->worker_count; i++) {
			worker *wrk;
			wrk = g_array_index(srv->workers, worker*, i);
			worker_stop(srv->main_worker, wrk);
		}
	}

	log_thread_wakeup(srv);
}

void server_exit(server *srv) {
	server_stop(srv);

	g_atomic_int_set(&srv->exiting, TRUE);

	/* exit all workers */
	{
		guint i;
		for (i = 0; i < srv->worker_count; i++) {
			worker *wrk;
			wrk = g_array_index(srv->workers, worker*, i);
			worker_exit(srv->main_worker, wrk);
		}
	}
}

/* cache timestamp */
GString *server_current_timestamp() {
	static GStaticPrivate last_ts_key = G_STATIC_PRIVATE_INIT;
	static GStaticPrivate ts_str_key = G_STATIC_PRIVATE_INIT;

	time_t *last_ts = g_static_private_get(&last_ts_key);
	GString *ts_str = g_static_private_get(&ts_str_key);

	time_t cur_ts = time(NULL);

	if (last_ts == NULL) {
		last_ts = g_new0(time_t, 1);
		g_static_private_set(&last_ts_key, last_ts, g_free);
	}
	if (ts_str == NULL) {
		ts_str = g_string_sized_new(255);
		g_static_private_set(&ts_str_key, ts_str, (GDestroyNotify)string_destroy_notify);
	}

	if (cur_ts != *last_ts) {
		gsize s;

		g_string_set_size(ts_str, 255);
		s = strftime(ts_str->str, ts_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&cur_ts));
		g_string_set_size(ts_str, s);
		*last_ts = cur_ts;
	}

	return ts_str;
}

void server_out_of_fds(server *srv) {
	ERROR(srv, "%s", "Too many open files. Either raise your fd limit or use a lower connection limit.");
}

guint server_ts_format_add(server *srv, GString* format) {
	/* check if not already registered */
	guint i;
	for (i = 0; i < srv->ts_formats->len; i++) {
		if (g_string_equal(g_array_index(srv->ts_formats, GString*, i), format))
			return i;
	}

	g_array_append_val(srv->ts_formats, format);
	return i;
}
