#pragma once

#include <stddef.h>

/* Maximum number of simultaneous listener clients */
#define SOCK_MAX_CLIENTS 16

/* ------------------------------------------------------------------ */
/* Server side (used by daemon)                                         */
/* ------------------------------------------------------------------ */

/* Create and bind the Unix socket. Returns fd on success, -1 on error.
 * Removes any stale socket file first. */
int sock_server_init(void);

/* Accept a pending connection. Returns client fd or -1 if none ready.
 * Non-blocking - call only when poll/epoll says the server fd is readable. */
int sock_server_accept(int server_fd);

/* Broadcast a newline-terminated JSON string to all connected clients.
 * Removes any client whose write fails (broken pipe). */
void sock_broadcast(int *clients, int *n_clients, const char *json);

/* ------------------------------------------------------------------ */
/* Client side (used by nyq listen)                                     */
/* ------------------------------------------------------------------ */

/* Connect to the daemon socket. Returns fd on success, -1 on error. */
int sock_client_connect(void);

/* Read and print lines from the socket, filtering by type if non-NULL.
 * Blocks until the connection closes. */
void sock_client_listen(int fd, const char *type_filter, const char *player_filter);

/* Clean up lock file (called on daemon exit) */
void sock_cleanup(void);
