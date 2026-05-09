#include "mpris.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define MPRIS_PREFIX     "org.mpris.MediaPlayer2."
#define MPRIS_PATH       "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_IF  "org.mpris.MediaPlayer2.Player"
#define MPRIS_PROPS_IF   "org.freedesktop.DBus.Properties"
#define DBUS_NAME        "org.freedesktop.DBus"
#define DBUS_PATH        "/org/freedesktop/DBus"

/* ------------------------------------------------------------------ */
/* Resolve player name -> full bus name                                 */
/* ------------------------------------------------------------------ */

static int resolve_player(sd_bus *bus, const char *name,
                          char *buf, int len) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_call_method(bus,
            DBUS_NAME, DBUS_PATH, DBUS_NAME,
            "ListNames", &err, &reply, NULL);
    if (r < 0) {
        fprintf(stderr, "nyq: ListNames failed: %s\n", err.message);
        sd_bus_error_free(&err);
        return -1;
    }

    r = sd_bus_message_enter_container(reply, 'a', "s");
    if (r < 0) { sd_bus_message_unref(reply); return -1; }

    const char *bname;
    int found = 0;
    while (sd_bus_message_read_basic(reply, 's', &bname) > 0) {
        if (strncmp(bname, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
            continue;
        const char *player = bname + strlen(MPRIS_PREFIX);
        if (!name || strstr(player, name)) {
            snprintf(buf, len, "%s", bname);
            found = 1;
            break;
        }
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    return found ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Read player state                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char   title[256];
    char   artist[256];
    char   status[32];
    double volume;
} PlayerState;


/* Fetch title and artist via a separate Get "Metadata" call.
 * Simpler to parse than the nested variant inside GetAll. */
static void read_metadata(sd_bus *bus, const char *busname, PlayerState *out) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PROPS_IF, "Get",
                           &err, &reply,
                           "ss", MPRIS_PLAYER_IF, "Metadata");
    sd_bus_error_free(&err);
    if (r < 0) return;

    /* v -> a{sv} */
    r = sd_bus_message_enter_container(reply, 'v', "a{sv}");
    if (r < 0) goto out;
    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (r < 0) { sd_bus_message_exit_container(reply); goto out; }

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *mkey;
        sd_bus_message_read_basic(reply, 's', &mkey);

        if (strcmp(mkey, "xesam:title") == 0) {
            const char *t = NULL;
            sd_bus_message_read(reply, "v", "s", &t);
            if (t) snprintf(out->title, sizeof(out->title), "%s", t);

        } else if (strcmp(mkey, "xesam:artist") == 0) {
            /* v contains as: enter variant, then enter array, read first
             * string, skip rest, exit array, exit variant */
            r = sd_bus_message_enter_container(reply, 'v', "as");
            if (r >= 0) {
                r = sd_bus_message_enter_container(reply, 'a', "s");
                if (r >= 0) {
                    const char *a = NULL;
                    if (sd_bus_message_read_basic(reply, 's', &a) > 0 && a)
                        snprintf(out->artist, sizeof(out->artist), "%s", a);
                    /* skip remaining artists */
                    while (sd_bus_message_read_basic(reply, 's', &a) > 0) {}
                    sd_bus_message_exit_container(reply); /* a */
                }
                sd_bus_message_exit_container(reply); /* v */
            }

        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply); /* a{sv} */
    sd_bus_message_exit_container(reply); /* v      */

out:
    sd_bus_message_unref(reply);
}

static int read_player_state(sd_bus *bus, const char *busname,
                             PlayerState *out) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    memset(out, 0, sizeof(*out));
    snprintf(out->status, sizeof(out->status), "Stopped");
    out->volume = 0.0;

    r = sd_bus_call_method(bus,
            busname, MPRIS_PATH, MPRIS_PROPS_IF,
            "GetAll", &err, &reply,
            "s", MPRIS_PLAYER_IF);
    if (r < 0) {
        fprintf(stderr, "nyq: GetAll failed: %s\n", err.message);
        sd_bus_error_free(&err);
        return -1;
    }

    /* top level: a{sv} */
    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (r < 0) goto out;

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key;
        sd_bus_message_read_basic(reply, 's', &key);

        if (strcmp(key, "PlaybackStatus") == 0) {
            const char *s = NULL;
            sd_bus_message_read(reply, "v", "s", &s);
            if (s) snprintf(out->status, sizeof(out->status), "%s", s);

        } else if (strcmp(key, "Volume") == 0) {
            sd_bus_message_read(reply, "v", "d", &out->volume);

        } else if (strcmp(key, "Metadata") == 0) {
            /* skip the whole Metadata variant then fetch fields separately */
            sd_bus_message_skip(reply, "v");

        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply); /* e */
    }
    sd_bus_message_exit_container(reply); /* a */

out:
    sd_bus_message_unref(reply);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Common entry point                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    MPRIS_CMD_STATUS,
    MPRIS_CMD_PLAY_PAUSE,
    MPRIS_CMD_NEXT,
    MPRIS_CMD_PREVIOUS,
    MPRIS_CMD_VOL_UP,
    MPRIS_CMD_VOL_DOWN,
} MprisCmd;

static const char *short_name(const char *busname) {
    const char *p = strstr(busname, MPRIS_PREFIX);
    if (p) return p + strlen(MPRIS_PREFIX);
    return busname;
}

static int mpris_oneshot_run(const char *name, MprisCmd cmd) {
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char busname[128] = {0};
    int r;

    r = sd_bus_open_user(&bus);
    if (r < 0) {
        fprintf(stderr, "nyq: sd_bus_open_user failed: %s\n", strerror(-r));
        return -1;
    }

    /* not found -> emit Stopped and exit cleanly */
    if (resolve_player(bus, name, busname, sizeof(busname)) < 0) {
        const char *display = name ? name : "player";
        emit_player(STDOUT_FILENO, display, "", "", "Stopped", 0.0);
        sd_bus_unref(bus);
        return 0;
    }

    const char *pname = short_name(busname);

    switch (cmd) {
    case MPRIS_CMD_PLAY_PAUSE:
        sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PLAYER_IF, "PlayPause",
                           &err, NULL, NULL);
        sd_bus_error_free(&err);
        break;
    case MPRIS_CMD_NEXT:
        sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PLAYER_IF, "Next",
                           &err, NULL, NULL);
        sd_bus_error_free(&err);
        break;
    case MPRIS_CMD_PREVIOUS:
        sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PLAYER_IF, "Previous",
                           &err, NULL, NULL);
        sd_bus_error_free(&err);
        break;
    case MPRIS_CMD_VOL_UP:
    case MPRIS_CMD_VOL_DOWN: {
        sd_bus_message *vreply = NULL;
        r = sd_bus_call_method(bus, busname, MPRIS_PATH,
                               MPRIS_PROPS_IF, "Get",
                               &err, &vreply,
                               "ss", MPRIS_PLAYER_IF, "Volume");
        sd_bus_error_free(&err);
        if (r >= 0) {
            double vol = 0.0;
            sd_bus_message_read(vreply, "v", "d", &vol);
            sd_bus_message_unref(vreply);
            vol += (cmd == MPRIS_CMD_VOL_UP) ? 0.05 : -0.05;
            if (vol > 1.0) vol = 1.0;
            if (vol < 0.0) vol = 0.0;
            sd_bus_set_property(bus, busname, MPRIS_PATH,
                                MPRIS_PLAYER_IF, "Volume",
                                &err, "d", vol);
            sd_bus_error_free(&err);
        }
        break;
    }
    case MPRIS_CMD_STATUS:
        break;
    }

    PlayerState state;
    if (read_player_state(bus, busname, &state) < 0) {
        emit_player(STDOUT_FILENO, pname, "", "", "Stopped", 0.0);
    } else {
        read_metadata(bus, busname, &state);
        emit_player(STDOUT_FILENO, pname,
                    state.title, state.artist,
                    state.status, state.volume);
    }

    sd_bus_unref(bus);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int mpris_oneshot_status(const char *name)     { return mpris_oneshot_run(name, MPRIS_CMD_STATUS);     }
int mpris_oneshot_play_pause(const char *name) { return mpris_oneshot_run(name, MPRIS_CMD_PLAY_PAUSE); }
int mpris_oneshot_next(const char *name)       { return mpris_oneshot_run(name, MPRIS_CMD_NEXT);       }
int mpris_oneshot_previous(const char *name)   { return mpris_oneshot_run(name, MPRIS_CMD_PREVIOUS);   }
int mpris_oneshot_vol_up(const char *name)     { return mpris_oneshot_run(name, MPRIS_CMD_VOL_UP);     }
int mpris_oneshot_vol_down(const char *name)   { return mpris_oneshot_run(name, MPRIS_CMD_VOL_DOWN);   }
