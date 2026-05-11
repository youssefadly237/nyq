#include "daemon.h"
#include "util.h"
#include "sock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>

#include <pipewire/pipewire.h>
#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>

#include <systemd/sd-bus.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAX_SINKS 32
#define MAX_PLAYERS 16
#define MAX_CLIENTS SOCK_MAX_CLIENTS
#define EVENT_QUEUE_SIZE 64
#define EVENT_JSON_SIZE 768

#define WAKEUP_BYTE 'e'

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_IF "org.mpris.MediaPlayer2.Player"
#define MPRIS_PROPS_IF "org.freedesktop.DBus.Properties"
#define DBUS_NAME "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"

/* ------------------------------------------------------------------ */
/* Event queue (PW thread -> main thread)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char json[EVENT_JSON_SIZE];
} Event;

typedef struct {
    Event items[EVENT_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t lock;
} EventQueue;

static EventQueue eq = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void eq_push(const char *json) {
    pthread_mutex_lock(&eq.lock);
    int next = (eq.tail + 1) % EVENT_QUEUE_SIZE;
    if (next != eq.head) {
        snprintf(eq.items[eq.tail].json, sizeof(eq.items[eq.tail].json), "%s", json);
        eq.tail = next;
    }
    pthread_mutex_unlock(&eq.lock);
}

static int eq_pop(char *out, int len) {
    pthread_mutex_lock(&eq.lock);
    if (eq.head == eq.tail) {
        pthread_mutex_unlock(&eq.lock);
        return 0;
    }
    snprintf(out, len, "%s", eq.items[eq.head].json);
    eq.head = (eq.head + 1) % EVENT_QUEUE_SIZE;
    pthread_mutex_unlock(&eq.lock);
    return 1;
}

/* ------------------------------------------------------------------ */
/* JSON helpers using cJSON                                             */
/* ------------------------------------------------------------------ */

/* Serialize a cJSON object to a newline-terminated string and push
 * it onto the event queue. Frees root. */
static void eq_push_json(cJSON *root) {
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str)
        return;

    /* append newline */
    size_t len = strlen(str);
    if (len + 2 < EVENT_JSON_SIZE) {
        char buf[EVENT_JSON_SIZE];
        memcpy(buf, str, len);
        buf[len] = '\n';
        buf[len + 1] = '\0';
        eq_push(buf);
    }
    cJSON_free(str);
}

static void push_sink_event(const char *name, float level, bool muted, bool is_default) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sink");
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddNumberToObject(root, "level", (double)level);
    cJSON_AddBoolToObject(root, "muted", muted);
    cJSON_AddStringToObject(root, "icon", volume_icon(level, muted));
    cJSON_AddBoolToObject(root, "default", is_default);
    eq_push_json(root);
}

static void push_sink_switch_event(const char *name) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sink-switch");
    cJSON_AddStringToObject(root, "name", name);
    eq_push_json(root);
}

static void push_player_event(const char *shortname, const char *title, const char *artist,
                              const char *status, double volume) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "player");
    cJSON_AddStringToObject(root, "name", shortname ? shortname : "");
    cJSON_AddStringToObject(root, "title", title ? title : "");
    cJSON_AddStringToObject(root, "artist", artist ? artist : "");
    cJSON_AddStringToObject(root, "status", status ? status : "Stopped");
    cJSON_AddNumberToObject(root, "volume", volume);
    eq_push_json(root);
}

/* ------------------------------------------------------------------ */
/* PipeWire daemon state                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id;
    uint32_t device_id;
    char name[256];
    struct pw_node *node;
    struct spa_hook node_listener;
    struct pw_device *device;
    struct spa_hook device_listener;
    int route_index;
    int card_device;
    float level;
    bool muted;
    uint32_t n_channels;
    bool subscribed;
} DaemonSink;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_registry *registry;

    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;

    struct pw_metadata *metadata;

    DaemonSink sinks[MAX_SINKS];
    int n_sinks;
    char default_name[256];

    int wakeup_fd;
} PwDaemon;

static PwDaemon *g_pw = NULL;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void pw_bind_sink_device(PwDaemon *d, DaemonSink *sink);
static void pw_subscribe_sink(DaemonSink *sink);

/* ------------------------------------------------------------------ */
/* Device param callback                                                */
/* ------------------------------------------------------------------ */

static void on_device_param_daemon(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                   const struct spa_pod *param) {
    DaemonSink *sink = data;
    (void)seq;
    (void)index;
    (void)next;
    if (id != SPA_PARAM_Route || !param)
        return;

    uint32_t route_idx = UINT32_MAX;
    uint32_t route_device = UINT32_MAX;
    uint32_t direction = UINT32_MAX;

    spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL, SPA_PARAM_ROUTE_index,
                         SPA_POD_OPT_Int(&route_idx), SPA_PARAM_ROUTE_device,
                         SPA_POD_OPT_Int(&route_device), SPA_PARAM_ROUTE_direction,
                         SPA_POD_OPT_Id(&direction));

    if (direction == (uint32_t)SPA_DIRECTION_OUTPUT && route_idx != UINT32_MAX &&
        route_device != UINT32_MAX) {
        sink->route_index = (int)route_idx;
        sink->card_device = (int)route_device;
    }
}

static const struct pw_device_events device_events_daemon = {
    PW_VERSION_DEVICE_EVENTS,
    .param = on_device_param_daemon,
};

/* ------------------------------------------------------------------ */
/* Node param callback                                                  */
/* ------------------------------------------------------------------ */

static void on_node_param_daemon(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                 const struct spa_pod *param) {
    DaemonSink *sink = data;
    (void)seq;
    (void)index;
    (void)next;
    if (id != SPA_PARAM_Props || !param)
        return;

    uint32_t n_vals = 0, val_size = 0, val_type = 0;
    const void *arr_body = NULL;
    bool muted = false;

    spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_channelVolumes,
                         SPA_POD_OPT_Array(&val_size, &val_type, &n_vals, &arr_body), SPA_PROP_mute,
                         SPA_POD_OPT_Bool(&muted));

    if (!arr_body || val_type != SPA_TYPE_Float || n_vals == 0)
        return;

    float *vols = (float *)arr_body;
    float new_level = linear_to_perceptual(vols[0]);

    if (new_level == sink->level && muted == sink->muted && sink->n_channels == n_vals)
        return;

    sink->level = new_level;
    sink->muted = muted;
    sink->n_channels = n_vals;

    if (!g_pw)
        return;

    bool is_default = strcmp(sink->name, g_pw->default_name) == 0;
    push_sink_event(sink->name, sink->level, sink->muted, is_default);

    char b = WAKEUP_BYTE;
    ssize_t r = write(g_pw->wakeup_fd, &b, 1);
    (void)r;
}

static const struct pw_node_events node_events_daemon = {
    PW_VERSION_NODE_EVENTS,
    .param = on_node_param_daemon,
};

/* ------------------------------------------------------------------ */
/* Metadata callback                                                    */
/* ------------------------------------------------------------------ */

static int on_metadata_property_daemon(void *data, uint32_t subject, const char *key,
                                       const char *type, const char *value) {
    PwDaemon *d = data;
    (void)type;
    if (subject != PW_ID_CORE || !key || !value)
        return 0;
    if (strcmp(key, "default.audio.sink") != 0)
        return 0;

    char new_name[256] = {0};
    parse_name_json(value, new_name, sizeof(new_name));
    if (strcmp(new_name, d->default_name) == 0)
        return 0;

    snprintf(d->default_name, sizeof(d->default_name), "%s", new_name);

    push_sink_switch_event(new_name);

    /* emit new default's current state */
    for (int i = 0; i < d->n_sinks; i++) {
        if (strcmp(d->sinks[i].name, new_name) == 0) {
            push_sink_event(d->sinks[i].name, d->sinks[i].level, d->sinks[i].muted, true);
            break;
        }
    }

    char b = WAKEUP_BYTE;
    ssize_t r = write(d->wakeup_fd, &b, 1);
    (void)r;
    return 0;
}

static const struct pw_metadata_events metadata_events_daemon = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property_daemon,
};

/* ------------------------------------------------------------------ */
/* Registry callbacks                                                   */
/* ------------------------------------------------------------------ */

static void pw_subscribe_sink(DaemonSink *sink) {
    if (sink->subscribed || !sink->node)
        return;
    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(sink->node, ids, 1);
    pw_node_enum_params(sink->node, 0, SPA_PARAM_Props, 0, 1, NULL);
    if (sink->device)
        pw_device_enum_params(sink->device, 0, SPA_PARAM_Route, 0, -1, NULL);
    sink->subscribed = true;
}

static void pw_bind_sink_device(PwDaemon *d, DaemonSink *sink) {
    sink->device = NULL;
    sink->route_index = -1;
    sink->card_device = -1;
    if (sink->device_id == 0)
        return;

    sink->device = (struct pw_device *)pw_registry_bind(
        d->registry, sink->device_id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
    if (sink->device)
        pw_device_add_listener(sink->device, &sink->device_listener, &device_events_daemon, sink);
}

static void on_global_daemon(void *data, uint32_t id, uint32_t permissions, const char *type,
                             uint32_t version, const struct spa_dict *props) {
    PwDaemon *d = data;
    (void)permissions;
    (void)version;

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && strcmp(name, "default") == 0 && !d->metadata) {
            d->metadata = pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Metadata,
                                           PW_VERSION_METADATA, 0);
            pw_metadata_add_listener(d->metadata, &d->metadata_listener, &metadata_events_daemon,
                                     d);
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!media_class || !node_name)
            return;
        if (strcmp(media_class, "Audio/Sink") != 0)
            return;
        if (d->n_sinks >= MAX_SINKS)
            return;

        DaemonSink *sink = &d->sinks[d->n_sinks++];
        memset(sink, 0, sizeof(*sink));
        sink->id = id;
        sink->route_index = -1;
        sink->card_device = -1;

        const char *dev_str = spa_dict_lookup(props, "device.id");
        sink->device_id = dev_str ? (uint32_t)strtoul(dev_str, NULL, 10) : 0;
        snprintf(sink->name, sizeof(sink->name), "%s", node_name);

        sink->node = pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (sink->node)
            pw_node_add_listener(sink->node, &sink->node_listener, &node_events_daemon, sink);

        pw_bind_sink_device(d, sink);
        pw_subscribe_sink(sink);
    }
}

static void on_global_remove_daemon(void *data, uint32_t id) {
    PwDaemon *d = data;
    for (int i = 0; i < d->n_sinks; i++) {
        if (d->sinks[i].id == id) {
            if (d->sinks[i].node) {
                spa_hook_remove(&d->sinks[i].node_listener);
                pw_proxy_destroy((struct pw_proxy *)d->sinks[i].node);
            }
            if (d->sinks[i].device) {
                spa_hook_remove(&d->sinks[i].device_listener);
                pw_proxy_destroy((struct pw_proxy *)d->sinks[i].device);
            }
            d->sinks[i] = d->sinks[--d->n_sinks];
            break;
        }
    }
}

static const struct pw_registry_events registry_events_daemon = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global_daemon,
    .global_remove = on_global_remove_daemon,
};

/* ------------------------------------------------------------------ */
/* MPRIS / D-Bus state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char busname[128];
    char unique_name[64];
    char shortname[64];
    char title[256];
    char artist[256];
    char status[32];
    double volume;
} PlayerState;

typedef struct {
    sd_bus *bus;
    sd_bus_slot *props_slot;
    sd_bus_slot *name_slot;
    PlayerState players[MAX_PLAYERS];
    int n_players;
    int wakeup_fd;
} BusDaemon;

/* ------------------------------------------------------------------ */
/* Player helpers                                                       */
/* ------------------------------------------------------------------ */

static PlayerState *bus_find_by_unique(BusDaemon *b, const char *unique) {
    for (int i = 0; i < b->n_players; i++)
        if (b->players[i].unique_name[0] && strcmp(b->players[i].unique_name, unique) == 0)
            return &b->players[i];
    return NULL;
}

static PlayerState *bus_add_player(BusDaemon *b, const char *busname) {
    if (b->n_players >= MAX_PLAYERS)
        return NULL;
    PlayerState *p = &b->players[b->n_players++];
    memset(p, 0, sizeof(*p));
    snprintf(p->busname, sizeof(p->busname), "%s", busname);
    snprintf(p->shortname, sizeof(p->shortname), "%s", busname + strlen(MPRIS_PREFIX));
    snprintf(p->status, sizeof(p->status), "Stopped");
    return p;
}

static void bus_resolve_unique(BusDaemon *b, PlayerState *p) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(b->bus, DBUS_NAME, DBUS_PATH, DBUS_NAME, "GetNameOwner", &err,
                               &reply, "s", p->busname);
    sd_bus_error_free(&err);
    if (r < 0)
        return;
    const char *owner = NULL;
    sd_bus_message_read(reply, "s", &owner);
    if (owner)
        snprintf(p->unique_name, sizeof(p->unique_name), "%s", owner);
    sd_bus_message_unref(reply);
}

static void bus_remove_player(BusDaemon *b, const char *busname) {
    for (int i = 0; i < b->n_players; i++) {
        if (strcmp(b->players[i].busname, busname) == 0) {
            push_player_event(b->players[i].shortname, "", "", "Stopped", 0.0);
            char wb = WAKEUP_BYTE;
            ssize_t r = write(b->wakeup_fd, &wb, 1);
            (void)r;
            b->players[i] = b->players[--b->n_players];
            return;
        }
    }
}

static void bus_emit_player(BusDaemon *b, PlayerState *p) {
    push_player_event(p->shortname, p->title, p->artist, p->status, p->volume);
    char wb = WAKEUP_BYTE;
    ssize_t r = write(b->wakeup_fd, &wb, 1);
    (void)r;
}

/* ------------------------------------------------------------------ */
/* Fetch player state from D-Bus                                        */
/* ------------------------------------------------------------------ */

static void bus_fetch_player_state(BusDaemon *b, PlayerState *p) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    /* GetAll for status + volume */
    r = sd_bus_call_method(b->bus, p->busname, MPRIS_PATH, MPRIS_PROPS_IF, "GetAll", &err, &reply,
                           "s", MPRIS_PLAYER_IF);
    sd_bus_error_free(&err);
    if (r < 0)
        return;

    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (r < 0)
        goto out;

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key;
        sd_bus_message_read_basic(reply, 's', &key);
        if (strcmp(key, "PlaybackStatus") == 0) {
            const char *s = NULL;
            sd_bus_message_read(reply, "v", "s", &s);
            if (s)
                snprintf(p->status, sizeof(p->status), "%s", s);
        } else if (strcmp(key, "Volume") == 0) {
            sd_bus_message_read(reply, "v", "d", &p->volume);
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
out:
    sd_bus_message_unref(reply);

    /* Get Metadata separately */
    reply = NULL;
    r = sd_bus_call_method(b->bus, p->busname, MPRIS_PATH, MPRIS_PROPS_IF, "Get", &err, &reply,
                           "ss", MPRIS_PLAYER_IF, "Metadata");
    sd_bus_error_free(&err);
    if (r < 0)
        return;

    r = sd_bus_message_enter_container(reply, 'v', "a{sv}");
    if (r < 0)
        goto out2;
    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (r < 0) {
        sd_bus_message_exit_container(reply);
        goto out2;
    }

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *mkey;
        sd_bus_message_read_basic(reply, 's', &mkey);
        if (strcmp(mkey, "xesam:title") == 0) {
            const char *t = NULL;
            sd_bus_message_read(reply, "v", "s", &t);
            if (t)
                snprintf(p->title, sizeof(p->title), "%s", t);
        } else if (strcmp(mkey, "xesam:artist") == 0) {
            r = sd_bus_message_enter_container(reply, 'v', "as");
            if (r >= 0) {
                r = sd_bus_message_enter_container(reply, 'a', "s");
                if (r >= 0) {
                    const char *a = NULL;
                    if (sd_bus_message_read_basic(reply, 's', &a) > 0 && a)
                        snprintf(p->artist, sizeof(p->artist), "%s", a);
                    while (sd_bus_message_read_basic(reply, 's', &a) > 0) {
                    }
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
out2:
    sd_bus_message_unref(reply);
}

/* ------------------------------------------------------------------ */
/* D-Bus signal handlers                                                */
/* ------------------------------------------------------------------ */

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    BusDaemon *b = userdata;
    (void)err;

    const char *sender = sd_bus_message_get_sender(m);
    if (!sender)
        return 0;

    /* sender is the unique bus name (:1.42) */
    PlayerState *p = bus_find_by_unique(b, sender);
    if (!p)
        return 0;

    const char *iface;
    sd_bus_message_read(m, "s", &iface);
    if (strcmp(iface, MPRIS_PLAYER_IF) != 0)
        return 0;

    bus_fetch_player_state(b, p);
    bus_emit_player(b, p);
    return 0;
}

static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    BusDaemon *b = userdata;
    (void)err;

    const char *name, *old_owner, *new_owner;
    sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);

    if (strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
        return 0;

    if (strlen(new_owner) > 0 && strlen(old_owner) == 0) {
        PlayerState *p = bus_add_player(b, name);
        if (p) {
            bus_resolve_unique(b, p);
            bus_fetch_player_state(b, p);
            bus_emit_player(b, p);
        }
    } else if (strlen(old_owner) > 0 && strlen(new_owner) == 0) {
        bus_remove_player(b, name);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Discover existing players at startup                                 */
/* ------------------------------------------------------------------ */

static void bus_discover_players(BusDaemon *b) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_call_method(b->bus, DBUS_NAME, DBUS_PATH, DBUS_NAME, "ListNames", &err, &reply,
                           NULL);
    sd_bus_error_free(&err);
    if (r < 0)
        return;

    r = sd_bus_message_enter_container(reply, 'a', "s");
    if (r < 0) {
        sd_bus_message_unref(reply);
        return;
    }

    const char *bname;
    while (sd_bus_message_read_basic(reply, 's', &bname) > 0) {
        if (strncmp(bname, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
            continue;

        sd_bus_message *sreply = NULL;
        sd_bus_error serr = SD_BUS_ERROR_NULL;
        r = sd_bus_call_method(b->bus, bname, MPRIS_PATH, MPRIS_PROPS_IF, "Get", &serr, &sreply,
                               "ss", MPRIS_PLAYER_IF, "PlaybackStatus");
        sd_bus_error_free(&serr);
        if (r < 0)
            continue;

        const char *status = NULL;
        sd_bus_message_read(sreply, "v", "s", &status);
        int active = status && (strcmp(status, "Playing") == 0 || strcmp(status, "Paused") == 0);
        sd_bus_message_unref(sreply);
        if (!active)
            continue;

        PlayerState *p = bus_add_player(b, bname);
        if (p) {
            bus_resolve_unique(b, p);
            bus_fetch_player_state(b, p);
        }
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Main daemon loop                                                     */
/* ------------------------------------------------------------------ */

int daemon_run(void) {
    int wakeup[2];
    if (pipe(wakeup) < 0) {
        perror("nyq: pipe");
        return -1;
    }

    pw_init(NULL, NULL);

    PwDaemon pw = {0};
    g_pw = &pw;
    pw.wakeup_fd = wakeup[1];

    pw.loop = pw_thread_loop_new("nyq-pw", NULL);
    if (!pw.loop) {
        fprintf(stderr, "nyq: pw_thread_loop_new failed\n");
        return -1;
    }

    pw.ctx = pw_context_new(pw_thread_loop_get_loop(pw.loop), NULL, 0);
    if (!pw.ctx) {
        fprintf(stderr, "nyq: pw_context_new failed\n");
        return -1;
    }

    pw_thread_loop_lock(pw.loop);

    pw.core = pw_context_connect(pw.ctx, NULL, 0);
    if (!pw.core) {
        pw_thread_loop_unlock(pw.loop);
        fprintf(stderr, "nyq: pw_context_connect failed\n");
        return -1;
    }

    pw.registry = pw_core_get_registry(pw.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(pw.registry, &pw.registry_listener, &registry_events_daemon, &pw);

    pw_thread_loop_unlock(pw.loop);
    pw_thread_loop_start(pw.loop);

    /* D-Bus */
    BusDaemon bus = {0};
    bus.wakeup_fd = wakeup[1];

    int r = sd_bus_open_user(&bus.bus);
    if (r < 0) {
        fprintf(stderr, "nyq: sd_bus_open_user failed: %s\n", strerror(-r));
        return -1;
    }

    sd_bus_match_signal(bus.bus, &bus.props_slot, NULL, MPRIS_PATH,
                        "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        on_properties_changed, &bus);
    sd_bus_match_signal(bus.bus, &bus.name_slot, DBUS_NAME, DBUS_PATH, DBUS_NAME,
                        "NameOwnerChanged", on_name_owner_changed, &bus);

    bus_discover_players(&bus);

    /* Socket */
    int server_fd = sock_server_init();
    if (server_fd < 0)
        return -1;

    int clients[MAX_CLIENTS];
    int n_clients = 0;

    /* epoll */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("nyq: epoll_create1");
        return -1;
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = wakeup[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, wakeup[0], &ev);

    int bus_fd = sd_bus_get_fd(bus.bus);
    ev.events = EPOLLIN;
    ev.data.fd = bus_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, bus_fd, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "nyq: daemon started\n");

    struct epoll_event events[16];
    while (g_running) {
        int n = epoll_wait(epfd, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == wakeup[0]) {
                char buf[64];
                ssize_t nr = read(wakeup[0], buf, sizeof(buf));
                (void)nr;
                char json[EVENT_JSON_SIZE];
                while (eq_pop(json, sizeof(json)))
                    sock_broadcast(clients, &n_clients, json);

            } else if (fd == bus_fd) {
                while (sd_bus_process(bus.bus, NULL) > 0) {
                }

            } else if (fd == server_fd) {
                int cfd = sock_server_accept(server_fd);
                if (cfd >= 0 && n_clients < MAX_CLIENTS) {
                    clients[n_clients++] = cfd;

                    /* send current state to new client */
                    pw_thread_loop_lock(pw.loop);
                    for (int j = 0; j < pw.n_sinks; j++) {
                        DaemonSink *sk = &pw.sinks[j];
                        bool is_def = strcmp(sk->name, pw.default_name) == 0;
                        /* build directly to avoid eq path */
                        cJSON *root = cJSON_CreateObject();
                        cJSON_AddStringToObject(root, "type", "sink");
                        cJSON_AddStringToObject(root, "name", sk->name);
                        cJSON_AddNumberToObject(root, "level", (double)sk->level);
                        cJSON_AddBoolToObject(root, "muted", sk->muted);
                        cJSON_AddStringToObject(root, "icon", volume_icon(sk->level, sk->muted));
                        cJSON_AddBoolToObject(root, "default", is_def);
                        char *str = cJSON_PrintUnformatted(root);
                        cJSON_Delete(root);
                        if (str) {
                            char line[EVENT_JSON_SIZE];
                            snprintf(line, sizeof(line), "%s\n", str);
                            cJSON_free(str);
                            int tmp_c[] = {cfd};
                            int tmp_n = 1;
                            sock_broadcast(tmp_c, &tmp_n, line);
                        }
                    }
                    pw_thread_loop_unlock(pw.loop);

                    for (int j = 0; j < bus.n_players; j++) {
                        PlayerState *p = &bus.players[j];
                        cJSON *root = cJSON_CreateObject();
                        cJSON_AddStringToObject(root, "type", "player");
                        cJSON_AddStringToObject(root, "name", p->shortname);
                        cJSON_AddStringToObject(root, "title", p->title);
                        cJSON_AddStringToObject(root, "artist", p->artist);
                        cJSON_AddStringToObject(root, "status", p->status);
                        cJSON_AddNumberToObject(root, "volume", p->volume);
                        char *str = cJSON_PrintUnformatted(root);
                        cJSON_Delete(root);
                        if (str) {
                            char line[EVENT_JSON_SIZE];
                            snprintf(line, sizeof(line), "%s\n", str);
                            cJSON_free(str);
                            int tmp_c[] = {cfd};
                            int tmp_n = 1;
                            sock_broadcast(tmp_c, &tmp_n, line);
                        }
                    }
                }
            }
        }
    }

    fprintf(stderr, "nyq: daemon stopping\n");

    for (int i = 0; i < n_clients; i++)
        close(clients[i]);
    close(server_fd);
    close(epfd);
    close(wakeup[0]);
    close(wakeup[1]);

    sd_bus_slot_unref(bus.props_slot);
    sd_bus_slot_unref(bus.name_slot);
    sd_bus_unref(bus.bus);

    pw_thread_loop_stop(pw.loop);
    pw_thread_loop_lock(pw.loop);
    for (int i = 0; i < pw.n_sinks; i++) {
        if (pw.sinks[i].node) {
            spa_hook_remove(&pw.sinks[i].node_listener);
            pw_proxy_destroy((struct pw_proxy *)pw.sinks[i].node);
        }
        if (pw.sinks[i].device) {
            spa_hook_remove(&pw.sinks[i].device_listener);
            pw_proxy_destroy((struct pw_proxy *)pw.sinks[i].device);
        }
    }
    if (pw.metadata) {
        spa_hook_remove(&pw.metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)pw.metadata);
    }
    spa_hook_remove(&pw.registry_listener);
    pw_proxy_destroy((struct pw_proxy *)pw.registry);
    pw_core_disconnect(pw.core);
    pw_thread_loop_unlock(pw.loop);
    pw_context_destroy(pw.ctx);
    pw_thread_loop_destroy(pw.loop);
    pw_deinit();

    return 0;
}
