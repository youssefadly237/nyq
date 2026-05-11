#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pw.h"
#include "mpris.h"
#include "daemon.h"
#include "sock.h"

static void usage(void) {
    fprintf(stderr, "usage: nyq <command> [args]\n"
                    "\n"
                    "daemon:\n"
                    "  daemon\n"
                    "\n"
                    "listen (requires daemon):\n"
                    "  listen [--type sink|player] [--player NAME]\n"
                    "\n"
                    "sink commands:\n"
                    "  sink-status\n"
                    "  sink-vol-up\n"
                    "  sink-vol-down\n"
                    "  sink-mute\n"
                    "  sink-next\n"
                    "  sink-prev\n"
                    "\n"
                    "player commands (NAME is optional partial match):\n"
                    "  player-status      [NAME]\n"
                    "  player-play-pause  [NAME]\n"
                    "  player-vol-up      [NAME]\n"
                    "  player-vol-down    [NAME]\n"
                    "  player-track-next  [NAME]\n"
                    "  player-track-prev  [NAME]\n"
                    "  player-cycle-next\n"
                    "  player-cycle-prev\n");
}

static int cmd_listen(int argc, char *argv[]) {
    const char *type_filter = NULL;
    const char *player_filter = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc)
            type_filter = argv[++i];
        else if (strcmp(argv[i], "--player") == 0 && i + 1 < argc)
            player_filter = argv[++i];
    }

    int fd = sock_client_connect();
    if (fd < 0)
        return 1;

    sock_client_listen(fd, type_filter, player_filter);
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    const char *name = argc >= 3 ? argv[2] : NULL;

    /* daemon */
    if (strcmp(cmd, "daemon") == 0)
        return daemon_run();

    /* listen */
    if (strcmp(cmd, "listen") == 0)
        return cmd_listen(argc, argv);

    /* sink */
    if (strcmp(cmd, "sink-status") == 0)
        return pw_oneshot_status();
    if (strcmp(cmd, "sink-vol-up") == 0)
        return pw_oneshot_vol_up();
    if (strcmp(cmd, "sink-vol-down") == 0)
        return pw_oneshot_vol_down();
    if (strcmp(cmd, "sink-mute") == 0)
        return pw_oneshot_mute();
    if (strcmp(cmd, "sink-next") == 0)
        return pw_oneshot_sink_next();
    if (strcmp(cmd, "sink-prev") == 0)
        return pw_oneshot_sink_prev();

    /* player */
    if (strcmp(cmd, "player-status") == 0)
        return mpris_oneshot_status(name);
    if (strcmp(cmd, "player-play-pause") == 0)
        return mpris_oneshot_play_pause(name);
    if (strcmp(cmd, "player-vol-up") == 0)
        return mpris_oneshot_vol_up(name);
    if (strcmp(cmd, "player-vol-down") == 0)
        return mpris_oneshot_vol_down(name);
    if (strcmp(cmd, "player-track-next") == 0)
        return mpris_oneshot_track_next(name);
    if (strcmp(cmd, "player-track-prev") == 0)
        return mpris_oneshot_track_prev(name);
    if (strcmp(cmd, "player-cycle-next") == 0)
        return mpris_oneshot_cycle_next();
    if (strcmp(cmd, "player-cycle-prev") == 0)
        return mpris_oneshot_cycle_prev();

    fprintf(stderr, "nyq: unknown command: %s\n", cmd);
    usage();
    return 1;
}
