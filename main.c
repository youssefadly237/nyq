#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pw.h"
#include "mpris.h"

static void usage(void) {
    fprintf(stderr,
        "usage: nyq <command> [args]\n"
        "\n"
        "sink commands:\n"
        "  sink-status\n"
        "  sink-raise\n"
        "  sink-lower\n"
        "  sink-mute\n"
        "\n"
        "player commands (NAME is optional partial match):\n"
        "  player-status    [NAME]\n"
        "  player-play-pause [NAME]\n"
        "  player-next      [NAME]\n"
        "  player-previous  [NAME]\n"
        "  player-vol-up    [NAME]\n"
        "  player-vol-down  [NAME]\n"
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd  = argv[1];
    const char *name = argc >= 3 ? argv[2] : NULL;

    /* sink */
    if (strcmp(cmd, "sink-status") == 0) return pw_oneshot_status();
    if (strcmp(cmd, "sink-raise")  == 0) return pw_oneshot_raise();
    if (strcmp(cmd, "sink-lower")  == 0) return pw_oneshot_lower();
    if (strcmp(cmd, "sink-mute")   == 0) return pw_oneshot_mute();

    /* player */
    if (strcmp(cmd, "player-status")     == 0) return mpris_oneshot_status(name);
    if (strcmp(cmd, "player-play-pause") == 0) return mpris_oneshot_play_pause(name);
    if (strcmp(cmd, "player-next")       == 0) return mpris_oneshot_next(name);
    if (strcmp(cmd, "player-previous")   == 0) return mpris_oneshot_previous(name);
    if (strcmp(cmd, "player-vol-up")     == 0) return mpris_oneshot_vol_up(name);
    if (strcmp(cmd, "player-vol-down")   == 0) return mpris_oneshot_vol_down(name);

    fprintf(stderr, "nyq: unknown command: %s\n", cmd);
    usage();
    return 1;
}
