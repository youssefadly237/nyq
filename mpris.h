#pragma once

/* One-shot MPRIS player operations via sd-bus.
 * NAME is a partial match: "spotify" matches "org.mpris.MediaPlayer2.spotify".
 * If NAME is NULL, the first available player is used.
 * All functions print JSON to stdout and return 0 on success, -1 on error.
 * If player is not running, prints a Stopped event and returns 0. */

int mpris_oneshot_status(const char *name);
int mpris_oneshot_play_pause(const char *name);
int mpris_oneshot_next(const char *name);
int mpris_oneshot_previous(const char *name);
int mpris_oneshot_vol_up(const char *name);
int mpris_oneshot_vol_down(const char *name);
