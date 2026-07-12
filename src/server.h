// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <ev.h>

typedef struct session session_t;
struct server_data;

#ifdef CONFIG_SERVER

struct server_data *server_init(session_t *ps);
void server_destroy(session_t *ps, struct server_data *sd);

#else

static inline struct server_data *server_init(session_t *ps attr_unused) {
	return NULL;
}
static inline void server_destroy(session_t *ps attr_unused,
                                  struct server_data *sd attr_unused) {
}

#endif
