// SPDX-License-Identifier: MIT
#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <uthash.h>
#include <ev.h>

#include "common.h"
#include "compiler.h"
#include "log.h"
#include "picom.h"
#include "utils/misc.h"

#define MAX_CMD_LEN 4096
#define MAX_RSP_LEN 4096

struct client_connection {
	int fd;
	struct ev_io read_watcher;
	struct ev_io write_watcher;
	char in_buf[MAX_CMD_LEN];
	size_t in_len;
	char out_buf[MAX_RSP_LEN];
	size_t out_len;
	size_t out_sent;
	bool close_after_write;
	struct client_connection *next;
	struct server_data *sd;
};

struct server_data {
	int listen_fd;
	struct ev_io accept_watcher;
	struct client_connection *clients;
	session_t *ps;
};

// ── helpers ──────────────────────────────────────────────────────────────────

static void send_response(struct client_connection *cc, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void send_response(struct client_connection *cc, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(cc->out_buf + cc->out_len,
	                  sizeof(cc->out_buf) - cc->out_len, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	cc->out_len += (size_t)n;
	if (cc->out_len > sizeof(cc->out_buf)) {
		cc->out_len = sizeof(cc->out_buf);
	}
	ev_io_start(cc->sd->ps->loop, &cc->write_watcher);
}

static void close_client(struct client_connection *cc) {
	ev_io_stop(cc->sd->ps->loop, &cc->read_watcher);
	ev_io_stop(cc->sd->ps->loop, &cc->write_watcher);
	close(cc->fd);

	// Unlink from list
	struct client_connection **prev = &cc->sd->clients;
	while (*prev && *prev != cc) {
		prev = &(*prev)->next;
	}
	if (*prev) {
		*prev = cc->next;
	}
	free(cc);
}

// ── command parsing and execution ─────────────────────────────────────────────

static void cmd_set(struct server_data *sd, struct client_connection *cc,
                    int argc, char **argv) {
	if (argc < 3) {
		send_response(cc, "ERR usage: SET <key> <type> <value1> [value2 ...]\n");
		return;
	}
	const char *key = argv[0];
	const char *type_name = argv[1];

	// Parse the type
	enum shader_uniform_type type = SU_FLOAT;
	if (strcmp(type_name, "float") == 0) {
		type = SU_FLOAT;
	} else if (strcmp(type_name, "int") == 0) {
		type = SU_INT;
	} else if (strcmp(type_name, "vec2") == 0) {
		type = SU_VEC2;
	} else if (strcmp(type_name, "vec3") == 0) {
		type = SU_VEC3;
	} else if (strcmp(type_name, "vec4") == 0) {
		type = SU_VEC4;
	} else if (strcmp(type_name, "bool") == 0) {
		type = SU_BOOL;
	} else {
		send_response(cc, "ERR unknown type: %s\n", type_name);
		return;
	}

	// Check that we have enough values
	int needed = 1;
	if (type == SU_VEC2) needed = 2;
	else if (type == SU_VEC3) needed = 3;
	else if (type == SU_VEC4) needed = 4;
	if (argc < 1 + 1 + needed) {
		send_response(cc, "ERR %s requires %d values\n", type_name, needed);
		return;
	}

	// Build the state value
	struct shader_state_value *sv = NULL;
	HASH_FIND_STR(sd->ps->shader_state, key, sv);
	if (!sv) {
		sv = ccalloc(1, struct shader_state_value);
		sv->key = strdup(key);
		HASH_ADD_STR(sd->ps->shader_state, key, sv);
	}
	sv->type = type;
	for (int i = 0; i < needed; i++) {
		char *end = NULL;
		if (type == SU_INT || type == SU_BOOL) {
			long val = strtol(argv[1 + 1 + i], &end, 0);
			if (type == SU_INT) sv->i = (int)val;
			else sv->i = val != 0;
		} else {
			sv->v[i] = strtof(argv[1 + 1 + i], &end);
		}
		if (end == argv[1 + 1 + i]) {
			send_response(cc, "ERR invalid value for %s: %s\n",
			              type_name, argv[1 + 1 + i]);
			return;
		}
	}

	force_repaint(sd->ps);
	send_response(cc, "OK\n");
}

static void cmd_del(struct server_data *sd, struct client_connection *cc,
                    int argc, char **argv) {
	if (argc < 1) {
		send_response(cc, "ERR usage: DEL <key>\n");
		return;
	}
	struct shader_state_value *sv = NULL;
	HASH_FIND_STR(sd->ps->shader_state, argv[0], sv);
	if (sv) {
		HASH_DEL(sd->ps->shader_state, sv);
		free(sv->key);
		free(sv);
		force_repaint(sd->ps);
	}
	send_response(cc, "OK\n");
}

static void cmd_enable(struct server_data *sd, struct client_connection *cc,
                       int argc, char **argv) {
	if (argc < 1) {
		send_response(cc, "ERR usage: ENABLE <shader_name>\n");
		return;
	}
	struct shader_folder_entry *fe = NULL;
	HASH_FIND_STR(sd->ps->shader_folder_entries, argv[0], fe);
	if (!fe) {
		send_response(cc, "ERR shader not found: %s\n", argv[0]);
		return;
	}
	fe->enabled = true;
	force_repaint(sd->ps);
	send_response(cc, "OK\n");
}

static void cmd_disable(struct server_data *sd, struct client_connection *cc,
                        int argc, char **argv) {
	if (argc < 1) {
		send_response(cc, "ERR usage: DISABLE <shader_name>\n");
		return;
	}
	struct shader_folder_entry *fe = NULL;
	HASH_FIND_STR(sd->ps->shader_folder_entries, argv[0], fe);
	if (!fe) {
		send_response(cc, "ERR shader not found: %s\n", argv[0]);
		return;
	}
	fe->enabled = false;
	force_repaint(sd->ps);
	send_response(cc, "OK\n");
}

static void cmd_clearstate(struct server_data *sd, struct client_connection *cc,
                           int argc, char **argv) {
	if (argc < 1) {
		send_response(cc, "ERR usage: CLEARSTATE <shader_name>\n");
		return;
	}
	struct shader_folder_entry *fe = NULL;
	HASH_FIND_STR(sd->ps->shader_folder_entries, argv[0], fe);
	if (!fe) {
		send_response(cc, "ERR shader not found: %s\n", argv[0]);
		return;
	}
	// Reset every input variable this shader uses to zero, so the next
	// render pass sets the GL uniforms back to their default values.
	struct shader_input_var *v, *vtmp;
	HASH_ITER(hh, fe->vars, v, vtmp) {
		struct shader_state_value *sv = NULL;
		HASH_FIND_STR(sd->ps->shader_state, v->name, sv);
		if (!sv) {
			sv = ccalloc(1, struct shader_state_value);
			sv->key = strdup(v->name);
			HASH_ADD_STR(sd->ps->shader_state, key, sv);
		}
		sv->type = v->type;
		sv->f = 0;
		sv->i = 0;
		sv->v[0] = sv->v[1] = sv->v[2] = sv->v[3] = 0;
	}
	force_repaint(sd->ps);
	send_response(cc, "OK\n");
}

static void cmd_list(struct server_data *sd, struct client_connection *cc,
                     int argc attr_unused, char **argv attr_unused) {
	struct shader_folder_entry *fe, *tmp;
	HASH_ITER(hh, sd->ps->shader_folder_entries, fe, tmp) {
		const char *status = fe->enabled ? "enabled" : "disabled";
		send_response(cc, "%s %s\n", fe->name, status);
		if (fe->info && fe->vars) {
			struct shader_input_var *v, *vtmp;
			HASH_ITER(hh, fe->vars, v, vtmp) {
				send_response(cc, "  var %s type=%d loc=%d\n",
				              v->name, v->type, v->location);
			}
		}
	}
	send_response(cc, ".\n");
}

static void cmd_get(struct server_data *sd, struct client_connection *cc,
                    int argc, char **argv) {
	if (argc < 1) {
		send_response(cc, "ERR usage: GET <key>\n");
		return;
	}
	struct shader_state_value *sv = NULL;
	HASH_FIND_STR(sd->ps->shader_state, argv[0], sv);
	if (!sv) {
		send_response(cc, "NOTFOUND\n");
		return;
	}
	switch (sv->type) {
	case SU_FLOAT: send_response(cc, "%g\n", sv->f); break;
	case SU_INT:
	case SU_BOOL: send_response(cc, "%d\n", sv->i); break;
	case SU_VEC2: send_response(cc, "%g %g\n", sv->v[0], sv->v[1]); break;
	case SU_VEC3:
		send_response(cc, "%g %g %g\n", sv->v[0], sv->v[1], sv->v[2]);
		break;
	case SU_VEC4:
		send_response(cc, "%g %g %g %g\n", sv->v[0], sv->v[1], sv->v[2],
		              sv->v[3]);
		break;
	}
}

// ── line processing ───────────────────────────────────────────────────────────

#define MAX_ARGS 16

static void process_line(struct server_data *sd, struct client_connection *cc,
                         const char *line) {
	char *args[MAX_ARGS];
	int argc = 0;
	const char *p = line;

	// Skip leading whitespace
	while (*p && isspace((unsigned char)*p)) p++;
	if (!*p || *p == '#') {
		return; // empty or comment
	}

	// Tokenize
	char buf[MAX_CMD_LEN];
	strncpy(buf, p, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *tok = strtok(buf, " \t");
	while (tok && argc < MAX_ARGS) {
		args[argc++] = tok;
		tok = strtok(NULL, " \t");
	}
	if (argc == 0) {
		return;
	}

	if (strcmp(args[0], "SET") == 0) {
		cmd_set(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "DEL") == 0) {
		cmd_del(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "ENABLE") == 0) {
		cmd_enable(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "DISABLE") == 0) {
		cmd_disable(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "CLEARSTATE") == 0) {
		cmd_clearstate(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "LIST") == 0) {
		cmd_list(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "GET") == 0) {
		cmd_get(sd, cc, argc - 1, args + 1);
	} else if (strcmp(args[0], "PING") == 0) {
		send_response(cc, "PONG\n");
	} else if (strcmp(args[0], "QUIT") == 0) {
		send_response(cc, "BYE\n");
		close_client(cc);
	} else {
		send_response(cc, "ERR unknown command: %s\n", args[0]);
	}
}

// ── libev callbacks ───────────────────────────────────────────────────────────

static void client_read_cb(EV_P_ ev_io *w, int revents attr_unused) {
	auto cc = container_of(w, struct client_connection, read_watcher);
	char buf[4096];
	ssize_t n = read(cc->fd, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}
		close_client(cc);
		return;
	}
	if (n == 0) {
		// A client such as `nc -N` half-closes after sending its command. Keep
		// the connection alive until the queued response has been written.
		cc->close_after_write = true;
		ev_io_stop(EV_A_ &cc->read_watcher);
		if (cc->out_len == cc->out_sent) {
			close_client(cc);
		}
		return;
	}

	// Append to input buffer and process lines
	bool overflowing = false;
	for (ssize_t i = 0; i < n; i++) {
		char c = buf[i];
		if (c == '\n') {
			if (!overflowing) {
				cc->in_buf[cc->in_len] = '\0';
				process_line(cc->sd, cc, cc->in_buf);
			}
			cc->in_len = 0;
			overflowing = false;
		} else if (!overflowing) {
			if (cc->in_len < sizeof(cc->in_buf) - 1) {
				cc->in_buf[cc->in_len++] = c;
			} else {
				// Line too long: drop the rest of it until the next
				// newline instead of recycling its tail as a bogus command.
				log_warn("Shader server client input line too long, dropping");
				overflowing = true;
			}
		}
	}
}

static void client_write_cb(EV_P_ ev_io *w, int revents attr_unused) {
	auto cc = container_of(w, struct client_connection, write_watcher);
	if (cc->out_sent >= cc->out_len) {
		ev_io_stop(EV_A_ w);
		if (cc->close_after_write) {
			close_client(cc);
			return;
		}
		cc->out_len = 0;
		cc->out_sent = 0;
		return;
	}
	ssize_t n = send(cc->fd, cc->out_buf + cc->out_sent,
	                 cc->out_len - cc->out_sent, MSG_NOSIGNAL);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}
		close_client(cc);
		return;
	}
	cc->out_sent += (size_t)n;
	if (cc->out_sent >= cc->out_len) {
		ev_io_stop(EV_A_ w);
		if (cc->close_after_write) {
			close_client(cc);
			return;
		}
		cc->out_len = 0;
		cc->out_sent = 0;
	}
}

static void accept_cb(EV_P_ ev_io *w, int revents attr_unused) {
	auto sd = container_of(w, struct server_data, accept_watcher);
	struct sockaddr_un addr;
	socklen_t addrlen = sizeof(addr);
	int client_fd = accept(sd->listen_fd, (struct sockaddr *)&addr, &addrlen);
	if (client_fd < 0) {
		log_warn("Shader server accept failed: %s", strerror(errno));
		return;
	}
	fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

	auto cc = ccalloc(1, struct client_connection);
	cc->fd = client_fd;
	cc->sd = sd;
	ev_io_init(&cc->read_watcher, client_read_cb, client_fd, EV_READ);
	ev_io_init(&cc->write_watcher, client_write_cb, client_fd, EV_WRITE);
	ev_io_start(EV_A_ &cc->read_watcher);
	// Write watcher started on demand

	cc->next = sd->clients;
	sd->clients = cc;
}

// ── public API ────────────────────────────────────────────────────────────────

struct server_data *server_init(session_t *ps) {
	const char *sock_path = ps->o.shader_server_socket;
	if (!sock_path || !sock_path[0]) {
		// Default: /run/user/$UID/picom.sock or /tmp/picom-$DISPLAY.sock
		const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
		const char *display = DisplayString(ps->c.dpy);
		// Extract display number from ":N" or "host:N"
		const char *display_num = display;
		while (*display_num && *display_num != ':') display_num++;
		if (*display_num == ':') {
			display_num++;    // skip the ':'
		} else if (!*display_num) {
			display_num = display;
		}
		// Plain char *: ownership is transferred to ps->o.shader_server_socket,
		// which is freed in options cleanup / server_destroy. Using scoped_charp
		// here would free the buffer at end of this block, leaving sock_path
		// dangling and causing bind() to create a garbage-named socket in cwd.
		char *path;
		if (runtime_dir) {
			size_t len = strlen(runtime_dir) + 12;
			path = ccalloc(len, char);
			snprintf(path, len, "%s/picom.sock", runtime_dir);
		} else {
			size_t len = strlen(display_num) + 20;
			path = ccalloc(len, char);
			snprintf(path, len, "/tmp/picom-%s.sock", display_num);
		}
		free(ps->o.shader_server_socket);
		ps->o.shader_server_socket = path;
		sock_path = ps->o.shader_server_socket;
	}

	// Remove old socket file if it exists
	unlink(sock_path);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		log_error("Failed to create shader server socket: %s", strerror(errno));
		return NULL;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_error("Failed to bind shader server socket %s: %s",
		          sock_path, strerror(errno));
		close(fd);
		return NULL;
	}

	// Make the socket accessible to other processes
	chmod(sock_path, 0666);

	if (listen(fd, 5) < 0) {
		log_error("Failed to listen on shader server socket: %s", strerror(errno));
		close(fd);
		return NULL;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	auto sd = ccalloc(1, struct server_data);
	sd->listen_fd = fd;
	sd->ps = ps;

	ev_io_init(&sd->accept_watcher, accept_cb, fd, EV_READ);
	ev_io_start(ps->loop, &sd->accept_watcher);

	log_info("Shader server listening on %s", sock_path);
	return sd;
}

void server_destroy(session_t *ps, struct server_data *sd) {
	if (!sd) {
		return;
	}
	ev_io_stop(ps->loop, &sd->accept_watcher);

	// Close all clients
	while (sd->clients) {
		close_client(sd->clients);
	}

	close(sd->listen_fd);
	unlink(ps->o.shader_server_socket);
	free(sd);
}
