#include "daemon.h"
#include "util.h"
#include "sock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
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

/* Constants */

#define MAX_SINKS 32
#define MAX_STREAMS 64
#define MAX_PW_CLIENTS 64
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

/* Event queue (PW thread -> main thread) */

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

/* JSON helpers using cJSON */

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

static void push_player_event_full(const char *shortname, const char *title, const char *artist,
                                   const char *status, double volume, bool has_muted, bool muted) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "player");
    cJSON_AddStringToObject(root, "name", shortname ? shortname : "");
    cJSON_AddStringToObject(root, "title", title ? title : "");
    cJSON_AddStringToObject(root, "artist", artist ? artist : "");
    cJSON_AddStringToObject(root, "status", status ? status : "Stopped");
    cJSON_AddNumberToObject(root, "volume", volume);
    if (has_muted)
        cJSON_AddBoolToObject(root, "muted", muted);
    eq_push_json(root);
}

static void push_player_event(const char *shortname, const char *title, const char *artist,
                              const char *status, double volume) {
    push_player_event_full(shortname, title, artist, status, volume, false, false);
}

/* PipeWire daemon state */

typedef struct {
    uint32_t id;
    pid_t pid;
    char app_name[128];
    char app_id[128];
    char process_binary[128];
} DaemonPwClient;

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
    uint32_t id;
    uint32_t client_id;
    pid_t pid;
    char name[128];
    char node_name[256];
    char title[256];
    char artist[256];
    struct pw_node *node;
    struct spa_hook node_listener;
    float level;
    bool muted;
    uint32_t n_channels;
    bool subscribed;
} DaemonStream;

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

    DaemonStream streams[MAX_STREAMS];
    int n_streams;

    DaemonPwClient clients[MAX_PW_CLIENTS];
    int n_pw_clients;

    int wakeup_fd;
} PwDaemon;

static PwDaemon *g_pw = NULL;

/* Forward declarations */

static void pw_bind_sink_device(PwDaemon *d, DaemonSink *sink);
static void pw_subscribe_sink(DaemonSink *sink);
static void pw_subscribe_stream(DaemonStream *stream);
static void push_stream_player_event(DaemonStream *stream, double volume, bool muted);

static void notify_main_thread(int fd) {
    char b = WAKEUP_BYTE;
    ssize_t r = write(fd, &b, 1);
    (void)r;
}

static DaemonPwClient *pw_find_client(PwDaemon *d, uint32_t id) {
    for (int i = 0; i < d->n_pw_clients; i++) {
        if (d->clients[i].id == id)
            return &d->clients[i];
    }
    return NULL;
}

static void pw_update_stream_metadata(PwDaemon *d, DaemonStream *stream,
                                      const struct spa_dict *props) {
    const char *app_name = props ? spa_dict_lookup(props, PW_KEY_APP_NAME) : NULL;
    const char *process_binary = props ? spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY) : NULL;
    const char *app_id = props ? spa_dict_lookup(props, PW_KEY_APP_ID) : NULL;
    const char *media_name = props ? spa_dict_lookup(props, PW_KEY_MEDIA_NAME) : NULL;
    const char *media_title = props ? spa_dict_lookup(props, PW_KEY_MEDIA_TITLE) : NULL;
    const char *media_artist = props ? spa_dict_lookup(props, PW_KEY_MEDIA_ARTIST) : NULL;
    const char *pid_str = props ? spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID) : NULL;

    if (pid_str && stream->pid == 0) {
        pid_t new_pid = (pid_t)strtol(pid_str, NULL, 10);
        if (new_pid > 0)
            stream->pid = new_pid;
    }

    DaemonPwClient *client = stream->client_id ? pw_find_client(d, stream->client_id) : NULL;
    if (!app_name && client && client->app_name[0])
        app_name = client->app_name;
    if (!process_binary && client && client->process_binary[0])
        process_binary = client->process_binary;
    if (!app_id && client && client->app_id[0])
        app_id = client->app_id;

    const char *src;
    if (app_name && app_name[0])
        src = app_name;
    else if (process_binary && process_binary[0])
        src = process_binary;
    else if (app_id && app_id[0])
        src = app_id;
    else if (media_name && media_name[0])
        src = media_name;
    else
        src = stream->node_name;

    size_t i = 0;
    for (; src[i] && i + 1 < sizeof(stream->name); i++)
        stream->name[i] = (char)tolower((unsigned char)src[i]);
    stream->name[i] = '\0';
    if (i == 0)
        snprintf(stream->name, sizeof(stream->name), "%s", "player");

    if (media_title && media_title[0]) {
        snprintf(stream->title, sizeof(stream->title), "%s", media_title);
    }
    if (media_artist && media_artist[0]) {
        snprintf(stream->artist, sizeof(stream->artist), "%s", media_artist);
    }
}

/* Device param callback */

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

/* Sink node param callback */

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
    bool muted = sink->muted;

    spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_channelVolumes,
                         SPA_POD_OPT_Array(&val_size, &val_type, &n_vals, &arr_body), SPA_PROP_mute,
                         SPA_POD_OPT_Bool(&muted));

    bool have_volumes = arr_body && val_type == SPA_TYPE_Float && n_vals > 0;
    if (!have_volumes && sink->n_channels == 0)
        return;

    float new_level = sink->level;
    uint32_t new_channels = sink->n_channels;
    if (have_volumes) {
        float *vols = (float *)arr_body;
        new_level = linear_to_perceptual(vols[0]);
        new_channels = n_vals;
    }

    if (new_level == sink->level && muted == sink->muted && sink->n_channels == new_channels)
        return;

    sink->level = new_level;
    sink->muted = muted;
    sink->n_channels = new_channels;

    if (!g_pw)
        return;

    bool is_default = strcmp(sink->name, g_pw->default_name) == 0;
    push_sink_event(sink->name, sink->level, sink->muted, is_default);
    notify_main_thread(g_pw->wakeup_fd);
}

static const struct pw_node_events node_events_daemon = {
    PW_VERSION_NODE_EVENTS,
    .param = on_node_param_daemon,
};

/* Stream node callbacks */

static void on_stream_param_daemon(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                   const struct spa_pod *param) {
    DaemonStream *stream = data;
    (void)seq;
    (void)index;
    (void)next;
    if (id != SPA_PARAM_Props || !param)
        return;

    uint32_t n_vals = 0, val_size = 0, val_type = 0;
    const void *arr_body = NULL;
    bool muted = stream->muted;

    spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_channelVolumes,
                         SPA_POD_OPT_Array(&val_size, &val_type, &n_vals, &arr_body), SPA_PROP_mute,
                         SPA_POD_OPT_Bool(&muted));

    bool have_volumes = arr_body && val_type == SPA_TYPE_Float && n_vals > 0;
    if (!have_volumes && stream->n_channels == 0)
        return;

    float new_level = stream->level;
    uint32_t new_channels = stream->n_channels;
    if (have_volumes) {
        float *vols = (float *)arr_body;
        new_level = linear_to_perceptual(vols[0]);
        new_channels = n_vals;
    }

    if (new_level == stream->level && muted == stream->muted && stream->n_channels == new_channels)
        return;

    stream->level = new_level;
    stream->muted = muted;
    stream->n_channels = new_channels;

    if (!g_pw)
        return;

    push_stream_player_event(stream, (double)stream->level, stream->muted);
    notify_main_thread(g_pw->wakeup_fd);
}

/* Re-run name resolution whenever PipeWire delivers updated node props.
 * This fires after on_global_daemon, so late-arriving application.name
 * values (e.g. Spotify's "audio-src" node) get fixed up before any
 * volume param callbacks fire. */
static void on_stream_info_daemon(void *data, const struct pw_node_info *info) {
    DaemonStream *stream = data;
    if (!info || !info->props || !g_pw)
        return;
    pw_update_stream_metadata(g_pw, stream, info->props);
}

static const struct pw_node_events stream_node_events_daemon = {
    PW_VERSION_NODE_EVENTS,
    .info = on_stream_info_daemon,
    .param = on_stream_param_daemon,
};

/* Metadata callback */

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

    notify_main_thread(d->wakeup_fd);
    return 0;
}

static const struct pw_metadata_events metadata_events_daemon = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property_daemon,
};

/* Registry callbacks */

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

static void pw_subscribe_stream(DaemonStream *stream) {
    if (stream->subscribed || !stream->node)
        return;
    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(stream->node, ids, 1);
    pw_node_enum_params(stream->node, 0, SPA_PARAM_Props, 0, 1, NULL);
    stream->subscribed = true;
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

    if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        if (d->n_pw_clients >= MAX_PW_CLIENTS)
            return;

        DaemonPwClient *client = &d->clients[d->n_pw_clients++];
        memset(client, 0, sizeof(*client));
        client->id = id;

        const char *pid_str = spa_dict_lookup(props, PW_KEY_SEC_PID);
        if (pid_str)
            client->pid = (pid_t)strtol(pid_str, NULL, 10);

        const char *app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
        const char *app_id = spa_dict_lookup(props, PW_KEY_APP_ID);
        const char *process_binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
        if (app_name) {
            size_t n = strlen(app_name);
            if (n >= sizeof(client->app_name))
                n = sizeof(client->app_name) - 1;
            memmove(client->app_name, app_name, n);
            client->app_name[n] = '\0';
        }
        if (app_id) {
            size_t n = strlen(app_id);
            if (n >= sizeof(client->app_id))
                n = sizeof(client->app_id) - 1;
            memmove(client->app_id, app_id, n);
            client->app_id[n] = '\0';
        }
        if (process_binary) {
            size_t n = strlen(process_binary);
            if (n >= sizeof(client->process_binary))
                n = sizeof(client->process_binary) - 1;
            memmove(client->process_binary, process_binary, n);
            client->process_binary[n] = '\0';
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!media_class)
            return;

        if (strcmp(media_class, "Stream/Output/Audio") == 0) {
            if (d->n_streams >= MAX_STREAMS)
                return;

            DaemonStream *stream = &d->streams[d->n_streams++];
            memset(stream, 0, sizeof(*stream));
            stream->id = id;

            const char *client_str = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
            stream->client_id = client_str ? (uint32_t)strtoul(client_str, NULL, 10) : 0;

            const char *s = node_name ? node_name : "player";
            size_t n = strlen(s);
            if (n >= sizeof(stream->node_name))
                n = sizeof(stream->node_name) - 1;
            memmove(stream->node_name, s, n);
            stream->node_name[n] = '\0';

            const char *pid_str = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
            stream->pid = pid_str ? (pid_t)strtol(pid_str, NULL, 10) : 0;

            pw_update_stream_metadata(d, stream, props);

            stream->node =
                pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
            if (stream->node)
                pw_node_add_listener(stream->node, &stream->node_listener,
                                     &stream_node_events_daemon, stream);

            pw_subscribe_stream(stream);
            return;
        }

        if (!node_name || strcmp(media_class, "Audio/Sink") != 0)
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

        size_t n = strlen(node_name);
        if (n >= sizeof(sink->name))
            n = sizeof(sink->name) - 1;
        memmove(sink->name, node_name, n);
        sink->name[n] = '\0';

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

    for (int i = 0; i < d->n_streams; i++) {
        if (d->streams[i].id == id) {
            if (d->streams[i].node) {
                spa_hook_remove(&d->streams[i].node_listener);
                pw_proxy_destroy((struct pw_proxy *)d->streams[i].node);
            }
            d->streams[i] = d->streams[--d->n_streams];
            break;
        }
    }

    for (int i = 0; i < d->n_pw_clients; i++) {
        if (d->clients[i].id == id) {
            d->clients[i] = d->clients[--d->n_pw_clients];
            break;
        }
    }
}

static const struct pw_registry_events registry_events_daemon = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global_daemon,
    .global_remove = on_global_remove_daemon,
};

/* MPRIS / D-Bus state */

typedef struct {
    char busname[128];
    char unique_name[64];
    pid_t pid;
    char shortname[64];
    char title[256];
    char artist[256];
    char status[32];
    double volume;
    bool has_stream_muted;
    bool stream_muted;
    bool needs_fetch; /* set by PW thread when metadata is missing; consumed on main thread */
} PlayerState;

typedef struct BusDaemon {
    sd_bus *bus;
    sd_bus_slot *props_slot;
    sd_bus_slot *name_slot;
    PlayerState players[MAX_PLAYERS];
    int n_players;
    int wakeup_fd;
} BusDaemon;

static BusDaemon *g_bus = NULL;
static pthread_mutex_t g_bus_lock = PTHREAD_MUTEX_INITIALIZER;

/* Player helpers */

static PlayerState *bus_find_by_unique(BusDaemon *b, const char *unique) {
    for (int i = 0; i < b->n_players; i++)
        if (b->players[i].unique_name[0] && strcmp(b->players[i].unique_name, unique) == 0)
            return &b->players[i];
    return NULL;
}

static PlayerState *bus_find_by_pid(BusDaemon *b, pid_t pid) {
    if (pid <= 0)
        return NULL;
    for (int i = 0; i < b->n_players; i++)
        if (b->players[i].pid == pid)
            return &b->players[i];
    return NULL;
}

static PlayerState *bus_find_by_name(BusDaemon *b, const char *name) {
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < b->n_players; i++) {
        const char *player_name = b->players[i].shortname;
        if (strcasecmp(player_name, name) == 0)
            return &b->players[i];
        size_t plen = strlen(player_name);
        if (plen > 0 && strncasecmp(name, player_name, plen) == 0 && name[plen] == '.')
            return &b->players[i];
        size_t nlen = strlen(name);
        if (nlen > 0 && strncasecmp(player_name, name, nlen) == 0 && player_name[nlen] == '.')
            return &b->players[i];
    }
    return NULL;
}

/* Called from the PW thread. Looks up the MPRIS player by pid first,
 * then falls back to name matching, copies whatever metadata we already
 * have, and marks needs_fetch if the title is still empty so the main
 * thread can do a proper D-Bus round-trip. */
static void push_stream_player_event(DaemonStream *stream, double volume, bool muted) {
    char out_name[128] = {0};
    char title[256] = {0};
    char artist[256] = {0};
    char status[32] = "Playing";
    bool matched = false;

    pid_t pid = stream->pid;
    const char *name = stream->name;
    uint32_t client_id = stream->client_id;

    snprintf(out_name, sizeof(out_name), "%s", name && name[0] ? name : "player");
    snprintf(title, sizeof(title), "%s", stream->title);
    snprintf(artist, sizeof(artist), "%s", stream->artist);

    pthread_mutex_lock(&g_bus_lock);
    if (g_bus) {
        PlayerState *p = NULL;
        DaemonPwClient *client = NULL;

        if (pid > 0)
            p = bus_find_by_pid(g_bus, pid);
        if (!p && client_id > 0 && g_pw) {
            client = pw_find_client(g_pw, client_id);
            if (client && client->pid > 0)
                p = bus_find_by_pid(g_bus, client->pid);
        }
        if (!p && name && name[0])
            p = bus_find_by_name(g_bus, name);
        if (!p && client && client->app_name[0])
            p = bus_find_by_name(g_bus, client->app_name);
        if (p) {
            snprintf(out_name, sizeof(out_name), "%s", p->shortname);
            snprintf(out_name, sizeof(out_name), "%s", p->shortname);
            if (p->title[0]) {
                snprintf(title, sizeof(title), "%s", p->title);
                snprintf(artist, sizeof(artist), "%s", p->artist);
            }
            snprintf(status, sizeof(status), "%s", p->status[0] ? p->status : "Stopped");
            p->volume = volume;
            p->has_stream_muted = true;
            p->stream_muted = muted;
            if (!p->title[0])
                p->needs_fetch = true;
            matched = true;
        }
    }
    pthread_mutex_unlock(&g_bus_lock);

    push_player_event_full(out_name, title, artist, matched ? status : "Playing", volume, true,
                           muted);
}

static PlayerState *bus_add_player(BusDaemon *b, const char *busname) {
    if (b->n_players >= MAX_PLAYERS)
        return NULL;
    PlayerState *p = &b->players[b->n_players++];
    memset(p, 0, sizeof(*p));
    snprintf(p->busname, sizeof(p->busname), "%s", busname);
    snprintf(p->shortname, sizeof(p->shortname), "%s", busname + strlen(MPRIS_PREFIX));
    snprintf(p->status, sizeof(p->status), "%s", "Stopped");
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

static void bus_resolve_pid(BusDaemon *b, PlayerState *p) {
    sd_bus_creds *creds = NULL;
    pid_t pid = 0;
    int r = sd_bus_get_name_creds(b->bus, p->busname, SD_BUS_CREDS_PID, &creds);
    if (r < 0)
        return;
    r = sd_bus_creds_get_pid(creds, &pid);
    sd_bus_creds_unref(creds);
    if (r >= 0)
        p->pid = pid;
}

static void bus_remove_player(BusDaemon *b, const char *busname) {
    for (int i = 0; i < b->n_players; i++) {
        if (strcmp(b->players[i].busname, busname) == 0) {
            push_player_event(b->players[i].shortname, "", "", "Stopped", 0.0);
            notify_main_thread(b->wakeup_fd);
            b->players[i] = b->players[--b->n_players];
            return;
        }
    }
}

static void bus_emit_player(BusDaemon *b, PlayerState *p) {
    push_player_event_full(p->shortname, p->title, p->artist, p->status, p->volume,
                           p->has_stream_muted, p->stream_muted);
    notify_main_thread(b->wakeup_fd);
}

/* Fetch player state from D-Bus */

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

/* If any player was matched by the PW thread but had no title yet,
 * fetch full metadata now and re-emit.  Called on the main thread only,
 * after draining the event queue, so D-Bus calls are safe here. */
static void bus_flush_pending_fetches(BusDaemon *b) {
    pthread_mutex_lock(&g_bus_lock);
    for (int i = 0; i < b->n_players; i++) {
        PlayerState *p = &b->players[i];
        if (!p->needs_fetch)
            continue;
        p->needs_fetch = false;
        bus_fetch_player_state(b, p);
        bus_emit_player(b, p); /* pushes to eq; next wakeup will broadcast it */
    }
    pthread_mutex_unlock(&g_bus_lock);
}

/* D-Bus signal handlers */

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    BusDaemon *b = userdata;
    (void)err;

    const char *sender = sd_bus_message_get_sender(m);
    if (!sender)
        return 0;

    const char *iface;
    sd_bus_message_read(m, "s", &iface);
    if (strcmp(iface, MPRIS_PLAYER_IF) != 0)
        return 0;

    pthread_mutex_lock(&g_bus_lock);
    PlayerState *p = bus_find_by_unique(b, sender);
    if (!p) {
        pthread_mutex_unlock(&g_bus_lock);
        return 0;
    }

    bus_fetch_player_state(b, p);
    bus_emit_player(b, p);
    pthread_mutex_unlock(&g_bus_lock);
    return 0;
}

static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    BusDaemon *b = userdata;
    (void)err;

    const char *name, *old_owner, *new_owner;
    sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);

    if (strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0)
        return 0;

    pthread_mutex_lock(&g_bus_lock);
    if (strlen(new_owner) > 0 && strlen(old_owner) == 0) {
        PlayerState *p = bus_add_player(b, name);
        if (p) {
            bus_resolve_unique(b, p);
            bus_resolve_pid(b, p);
            bus_fetch_player_state(b, p);
            bus_emit_player(b, p);
        }
    } else if (strlen(old_owner) > 0 && strlen(new_owner) == 0) {
        bus_remove_player(b, name);
    }
    pthread_mutex_unlock(&g_bus_lock);
    return 0;
}

/* Discover existing players at startup */

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

        pthread_mutex_lock(&g_bus_lock);
        PlayerState *p = bus_add_player(b, bname);
        if (p) {
            bus_resolve_unique(b, p);
            bus_resolve_pid(b, p);
            bus_fetch_player_state(b, p);
        }
        pthread_mutex_unlock(&g_bus_lock);
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
}

/* Signal handling */

static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Main daemon loop */

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
    pthread_mutex_lock(&g_bus_lock);
    g_bus = &bus;
    pthread_mutex_unlock(&g_bus_lock);

    int r = sd_bus_open_user(&bus.bus);
    if (r < 0) {
        fprintf(stderr, "nyq: sd_bus_open_user failed: %s\n", strerror(-r));
        pthread_mutex_lock(&g_bus_lock);
        g_bus = NULL;
        pthread_mutex_unlock(&g_bus_lock);
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
                /* After draining the queue, do any deferred D-Bus fetches.
                 * bus_emit_player will write to the wakeup pipe again, so
                 * the freshly-fetched metadata gets broadcast on the next
                 * iteration - no busy loop because needs_fetch is cleared
                 * before the fetch, so the second wakeup finds nothing to do. */
                bus_flush_pending_fetches(&bus);

            } else if (fd == bus_fd) {
                while (sd_bus_process(bus.bus, NULL) > 0) {
                }

            } else if (fd == server_fd) {
                int cfd = sock_server_accept(server_fd);
                if (cfd >= 0 && n_clients < MAX_CLIENTS) {
                    clients[n_clients++] = cfd;

                    /* Send current state to the newly connected client */
                    pw_thread_loop_lock(pw.loop);
                    for (int j = 0; j < pw.n_sinks; j++) {
                        DaemonSink *sk = &pw.sinks[j];
                        bool is_def = strcmp(sk->name, pw.default_name) == 0;
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

                    pthread_mutex_lock(&g_bus_lock);
                    for (int j = 0; j < bus.n_players; j++) {
                        PlayerState *p = &bus.players[j];
                        cJSON *root = cJSON_CreateObject();
                        cJSON_AddStringToObject(root, "type", "player");
                        cJSON_AddStringToObject(root, "name", p->shortname);
                        cJSON_AddStringToObject(root, "title", p->title);
                        cJSON_AddStringToObject(root, "artist", p->artist);
                        cJSON_AddStringToObject(root, "status", p->status);
                        cJSON_AddNumberToObject(root, "volume", p->volume);
                        if (p->has_stream_muted)
                            cJSON_AddBoolToObject(root, "muted", p->stream_muted);
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
                    pthread_mutex_unlock(&g_bus_lock);
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

    pthread_mutex_lock(&g_bus_lock);
    g_bus = NULL;
    pthread_mutex_unlock(&g_bus_lock);

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
    for (int i = 0; i < pw.n_streams; i++) {
        if (pw.streams[i].node) {
            spa_hook_remove(&pw.streams[i].node_listener);
            pw_proxy_destroy((struct pw_proxy *)pw.streams[i].node);
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