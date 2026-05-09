#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "pw.h"

static void usage(void) {
    fprintf(stderr,
        "usage: nyq <command>\n"
        "  sink-status\n"
        "  sink-raise\n"
        "  sink-lower\n"
        "  sink-mute\n"
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "sink-status") == 0) return pw_oneshot_status();
    if (strcmp(cmd, "sink-raise")  == 0) return pw_oneshot_raise();
    if (strcmp(cmd, "sink-lower")  == 0) return pw_oneshot_lower();
    if (strcmp(cmd, "sink-mute")   == 0) return pw_oneshot_mute();

    fprintf(stderr, "nyq: unknown command: %s\n", cmd);
    usage();
    return 1;
}
