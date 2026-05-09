#include "mpris.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <systemd/sd-bus.h>

#define MPRIS_PREFIX     "org.mpris.MediaPlayer2."
#define MPRIS_PATH       "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_IF  "org.mpris.MediaPlayer2.Player"
#define MPRIS_PROPS_IF   "org.freedesktop.DBus.Properties"
#define DBUS_NAME        "org.freedesktop.DBus"
#define DBUS_PATH        "/org/freedesktop/DBus"

/* ------------------------------------------------------------------ */
/* State file                                                           */
/* ------------------------------------------------------------------ */

static const char *state_path(void) {
    static char path[256];
    if (path[0]) return path;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) runtime = "/tmp";
    snprintf(path, sizeof(path), "%s/nyq.state", runtime);
    return path;
}

static void state_write(const char *player_name) {
    FILE *f = fopen(state_path(), "w");
    if (!f) return;
    fprintf(f, "%s\n", player_name);
    fclose(f);
}

static void state_read(char *buf, int len) {
    buf[0] = '\0';
    FILE *f = fopen(state_path(), "r");
    if (!f) return;
    if (fgets(buf, len, f)) {
        /* strip newline */
        int l = strlen(buf);
        if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
    }
    fclose(f);
}

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
/* List active players (Playing or Paused)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char busname[128];
    char shortname[64];
} PlayerEntry;

static int list_active_players(sd_bus *bus, PlayerEntry *out, int max) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r, count = 0;

    r = sd_bus_call_method(bus,
            DBUS_NAME, DBUS_PATH, DBUS_NAME,
            "ListNames", &err, &reply, NULL);
    if (r < 0) {
        sd_bus_error_free(&err);
        return 0;
    }

    r = sd_bus_message_enter_container(reply, 'a', "s");
    if (r < 0) { sd_bus_message_unref(reply); return 0; }

    const char *bname;
    while (sd_bus_message_read_basic(reply, 's', &bname) > 0 &&
           count < max) {
        if (strncmp(bname, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
            continue;

        /* check status */
        sd_bus_message *sreply = NULL;
        sd_bus_error serr = SD_BUS_ERROR_NULL;
        r = sd_bus_call_method(bus, bname, MPRIS_PATH,
                               MPRIS_PROPS_IF, "Get",
                               &serr, &sreply,
                               "ss", MPRIS_PLAYER_IF, "PlaybackStatus");
        sd_bus_error_free(&serr);
        if (r < 0) continue;

        const char *status = NULL;
        sd_bus_message_read(sreply, "v", "s", &status);

        int active = status &&
                     (strcmp(status, "Playing") == 0 ||
                      strcmp(status, "Paused")  == 0);
        sd_bus_message_unref(sreply);

        if (!active) continue;

        snprintf(out[count].busname,   sizeof(out[count].busname),
                 "%s", bname);
        snprintf(out[count].shortname, sizeof(out[count].shortname),
                 "%s", bname + strlen(MPRIS_PREFIX));
        count++;
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    return count;
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

static void read_metadata(sd_bus *bus, const char *busname,
                          PlayerState *out) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PROPS_IF, "Get",
                           &err, &reply,
                           "ss", MPRIS_PLAYER_IF, "Metadata");
    sd_bus_error_free(&err);
    if (r < 0) return;

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
            r = sd_bus_message_enter_container(reply, 'v', "as");
            if (r >= 0) {
                r = sd_bus_message_enter_container(reply, 'a', "s");
                if (r >= 0) {
                    const char *a = NULL;
                    if (sd_bus_message_read_basic(reply, 's', &a) > 0 && a)
                        snprintf(out->artist, sizeof(out->artist), "%s", a);
                    while (sd_bus_message_read_basic(reply, 's', &a) > 0) {}
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_exit_container(reply);

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

    r = sd_bus_call_method(bus, busname, MPRIS_PATH, MPRIS_PROPS_IF,
                           "GetAll", &err, &reply, "s", MPRIS_PLAYER_IF);
    if (r < 0) {
        sd_bus_error_free(&err);
        return -1;
    }

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
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);

out:
    sd_bus_message_unref(reply);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Emit helper                                                          */
/* ------------------------------------------------------------------ */

static void emit_state(sd_bus *bus, const char *busname,
                       const char *shortname) {
    PlayerState state;
    if (read_player_state(bus, busname, &state) < 0) {
        emit_player(STDOUT_FILENO, shortname, "", "", "Stopped", 0.0);
        return;
    }
    read_metadata(bus, busname, &state);
    emit_player(STDOUT_FILENO, shortname,
                state.title, state.artist, state.status, state.volume);
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    MPRIS_CMD_STATUS,
    MPRIS_CMD_PLAY_PAUSE,
    MPRIS_CMD_TRACK_NEXT,
    MPRIS_CMD_TRACK_PREV,
    MPRIS_CMD_VOL_UP,
    MPRIS_CMD_VOL_DOWN,
    MPRIS_CMD_CYCLE_NEXT,
    MPRIS_CMD_CYCLE_PREV,
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

    /* cycle commands use their own logic */
    if (cmd == MPRIS_CMD_CYCLE_NEXT || cmd == MPRIS_CMD_CYCLE_PREV) {
        PlayerEntry players[16];
        int n = list_active_players(bus, players, 16);

        if (n == 0) {
            emit_player(STDOUT_FILENO, "player", "", "", "Stopped", 0.0);
            sd_bus_unref(bus);
            return 0;
        }

        /* find current in state file */
        char cur[64] = {0};
        state_read(cur, sizeof(cur));

        int cur_idx = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(players[i].shortname, cur) == 0) {
                cur_idx = i;
                break;
            }
        }

        int dir = (cmd == MPRIS_CMD_CYCLE_NEXT) ? +1 : -1;
        int next_idx = (cur_idx + dir + n) % n;

        state_write(players[next_idx].shortname);
        emit_state(bus, players[next_idx].busname,
                   players[next_idx].shortname);
        sd_bus_unref(bus);
        return 0;
    }

    /* all other commands: resolve player first */

    /* for named commands, check state file if no name given */
    char state_name[64] = {0};
    if (!name) {
        state_read(state_name, sizeof(state_name));
        if (state_name[0]) name = state_name;
    }

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
    case MPRIS_CMD_TRACK_NEXT:
        sd_bus_call_method(bus, busname, MPRIS_PATH,
                           MPRIS_PLAYER_IF, "Next",
                           &err, NULL, NULL);
        sd_bus_error_free(&err);
        break;
    case MPRIS_CMD_TRACK_PREV:
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
    default:
        break;
    }

    /* update state file with resolved player */
    state_write(pname);
    emit_state(bus, busname, pname);

    sd_bus_unref(bus);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int mpris_oneshot_status(const char *name)      { return mpris_oneshot_run(name, MPRIS_CMD_STATUS);      }
int mpris_oneshot_play_pause(const char *name)  { return mpris_oneshot_run(name, MPRIS_CMD_PLAY_PAUSE);  }
int mpris_oneshot_track_next(const char *name)  { return mpris_oneshot_run(name, MPRIS_CMD_TRACK_NEXT);  }
int mpris_oneshot_track_prev(const char *name)  { return mpris_oneshot_run(name, MPRIS_CMD_TRACK_PREV);  }
int mpris_oneshot_vol_up(const char *name)      { return mpris_oneshot_run(name, MPRIS_CMD_VOL_UP);      }
int mpris_oneshot_vol_down(const char *name)    { return mpris_oneshot_run(name, MPRIS_CMD_VOL_DOWN);    }
int mpris_oneshot_cycle_next(void)              { return mpris_oneshot_run(NULL, MPRIS_CMD_CYCLE_NEXT);  }
int mpris_oneshot_cycle_prev(void)              { return mpris_oneshot_run(NULL, MPRIS_CMD_CYCLE_PREV);  }
