#include "sock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cJSON.h>

/* Socket path */

static const char *runtime_dir(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        fprintf(stderr, "nyq: XDG_RUNTIME_DIR not set\n");
    return runtime;
}

static const char *sock_path(void) {
    static char path[108];
    if (path[0])
        return path;

    const char *runtime = runtime_dir();
    if (!runtime)
        return NULL;
    snprintf(path, sizeof(path), "%s/nyq.sock", runtime);
    return path;
}

static const char *lock_path(void) {
    static char path[108];
    if (path[0])
        return path;

    const char *runtime = runtime_dir();
    if (!runtime)
        return NULL;
    snprintf(path, sizeof(path), "%s/nyq.lock", runtime);
    return path;
}

void sock_cleanup(void) {
    const char *path = lock_path();
    if (path)
        unlink(path);
}

/* Server */

int sock_server_init(void) {
    const char *lockfile = lock_path();
    if (!lockfile)
        return -1;

    /* Check for existing lock */
    FILE *lock = fopen(lockfile, "r");
    if (lock) {
        pid_t old_pid = 0;
        if (fscanf(lock, "%d", &old_pid) == 1 && old_pid > 0) {
            /* Check if process is actually running */
            if (kill(old_pid, 0) == 0) {
                fclose(lock);
                fprintf(stderr, "nyq: daemon already running (pid %d)\n", old_pid);
                return -1;
            }
        }
        fclose(lock);
    }

    /* Write our PID to lock file */
    lock = fopen(lockfile, "w");
    if (!lock) {
        perror("nyq: lock file");
        return -1;
    }
    fprintf(lock, "%d\n", getpid());
    fclose(lock);

    const char *path = sock_path();
    if (!path)
        return -1;

    /* remove stale socket */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("nyq: socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("nyq: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        perror("nyq: listen");
        close(fd);
        return -1;
    }

    return fd;
}

int sock_server_accept(int server_fd) {
    int client = accept(server_fd, NULL, NULL);
    if (client < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("nyq: accept");
        return -1;
    }
    return client;
}

void sock_broadcast(int *clients, int *n_clients, const char *json) {
    int len = strlen(json);
    int i = 0;

    while (i < *n_clients) {
        int fd = clients[i];
        ssize_t w = write(fd, json, len);

        if (w < 0) {
            /* client disconnected - remove from list */
            close(fd);
            clients[i] = clients[*n_clients - 1];
            (*n_clients)--;
            /* do not increment i - recheck this slot */
        } else {
            i++;
        }
    }
}

/* Client */

int sock_client_connect(void) {
    const char *path = sock_path();
    if (!path)
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("nyq: socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "nyq: cannot connect to daemon at %s: %s\n"
                "nyq: is 'nyq daemon' running?\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

void sock_client_listen(int fd, const char *type_filter, const char *player_filter) {
    char buf[2048];
    char line[2048];
    int line_len = 0;

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break; /* daemon closed or error */

        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];

            if (c == '\n') {
                line[line_len] = '\0';

                /* apply filters if set */
                int pass = 1;

                if (pass && (type_filter || player_filter)) {
                    cJSON *root = cJSON_Parse(line);
                    if (root) {
                        if (type_filter) {
                            cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "type");
                            if (!cJSON_IsString(t) || strcmp(t->valuestring, type_filter) != 0)
                                pass = 0;
                        }
                        if (pass && player_filter) {
                            cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
                            if (!cJSON_IsString(nm) ||
                                strstr(nm->valuestring, player_filter) == NULL)
                                pass = 0;
                        }
                        cJSON_Delete(root);
                    } else {
                        pass = 0;
                    }
                }

                if (pass) {
                    puts(line);
                    fflush(stdout);
                }

                line_len = 0;
            } else {
                if (line_len < (int)sizeof(line) - 1)
                    line[line_len++] = c;
            }
        }
    }
}
