#include "pw.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <cJSON.h>

/* ------------------------------------------------------------------ */
/* Sink registry                                                        */
/* ------------------------------------------------------------------ */

#define MAX_SINKS 32

typedef struct {
    uint32_t id;
    uint32_t device_id; /* parent device object id, 0 = none */
    char name[256];
} SinkEntry;

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    PHASE_WAIT_GLOBALS,  /* initial registry burst + metadata properties */
    PHASE_WAIT_METADATA, /* extra roundtrip to ensure metadata arrived    */
    PHASE_WAIT_BIND,     /* node + device bind roundtrip                  */
    PHASE_WAIT_ROUTES,   /* device Route enum roundtrip                   */
    PHASE_WAIT_PARAMS,   /* node Props enum                               */
    PHASE_DONE,
} Phase;

typedef enum {
    PW_CMD_STATUS,
    PW_CMD_VOL_UP,
    PW_CMD_VOL_DOWN,
    PW_CMD_MUTE,
    PW_CMD_SINK_NEXT,
    PW_CMD_SINK_PREV,
} PwCmd;

typedef struct {
    struct pw_main_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_registry *registry;

    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;
    struct spa_hook node_listener;
    struct spa_hook device_listener;

    struct pw_metadata *metadata;
    struct pw_node *node;
    struct pw_device *device;

    SinkEntry sinks[MAX_SINKS];
    int n_sinks;

    uint32_t target_id;
    char target_name[256];

    /* props read from node */
    float level;
    bool muted;
    uint32_t n_channels;

    /* route info read from device */
    int route_index; /* -1 = unknown */
    int card_device; /* -1 = unknown */

    PwCmd cmd;
    int sync_seq;
    Phase phase;
    int error;
} PwState;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void start_bind(PwState *s);
static void start_routes(PwState *s);
static void start_params(PwState *s);
static void finish(PwState *s);

/* ------------------------------------------------------------------ */
/* Core events                                                          */
/* ------------------------------------------------------------------ */

static void on_core_done(void *data, uint32_t id, int seq) {
    PwState *s = data;
    if (id != PW_ID_CORE || seq != s->sync_seq)
        return;

    switch (s->phase) {
    case PHASE_WAIT_GLOBALS:
        /* extra roundtrip to ensure metadata properties have arrived */
        s->phase = PHASE_WAIT_METADATA;
        s->sync_seq = pw_core_sync(s->core, PW_ID_CORE, 0);
        break;
    case PHASE_WAIT_METADATA:
        s->phase = PHASE_WAIT_BIND;
        start_bind(s);
        break;
    case PHASE_WAIT_BIND:
        s->phase = PHASE_WAIT_ROUTES;
        start_routes(s);
        break;
    case PHASE_WAIT_ROUTES:
        s->phase = PHASE_WAIT_PARAMS;
        start_params(s);
        break;
    case PHASE_WAIT_PARAMS:
        /* no Props param arrived — finish with level=0 */
        finish(s);
        break;
    default:
        break;
    }
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    PwState *s = data;
    fprintf(stderr, "nyq: pipewire error id=%u seq=%d res=%d: %s\n", id, seq, res, message);
    s->error = res;
    pw_main_loop_quit(s->loop);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

/* ------------------------------------------------------------------ */
/* Metadata events                                                      */
/* ------------------------------------------------------------------ */

static int on_metadata_property(void *data, uint32_t subject, const char *key, const char *type,
                                const char *value) {
    PwState *s = data;
    (void)type;
    if (subject != PW_ID_CORE || !key || !value)
        return 0;
    if (strcmp(key, "default.audio.sink") != 0)
        return 0;
    parse_name_json(value, s->target_name, sizeof(s->target_name));
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

/* ------------------------------------------------------------------ */
/* Device param event — read Route index + card device                 */
/* ------------------------------------------------------------------ */

static void on_device_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                            const struct spa_pod *param) {
    PwState *s = data;
    (void)seq;
    (void)index;
    (void)next;
    if (id != SPA_PARAM_Route || !param)
        return;

    uint32_t route_idx = UINT32_MAX, route_device = UINT32_MAX, direction = UINT32_MAX;
    spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL, SPA_PARAM_ROUTE_index,
                         SPA_POD_OPT_Int(&route_idx), SPA_PARAM_ROUTE_device,
                         SPA_POD_OPT_Int(&route_device), SPA_PARAM_ROUTE_direction,
                         SPA_POD_OPT_Id(&direction));

    if (direction == (uint32_t)SPA_DIRECTION_OUTPUT && route_idx != UINT32_MAX &&
        route_device != UINT32_MAX) {
        s->route_index = (int)route_idx;
        s->card_device = (int)route_device;
    }
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .param = on_device_param,
};

/* ------------------------------------------------------------------ */
/* Node param event                                                     */
/* ------------------------------------------------------------------ */

static void on_node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                          const struct spa_pod *param) {
    PwState *s = data;
    (void)seq;
    (void)index;
    (void)next;
    if (id != SPA_PARAM_Props || s->phase == PHASE_DONE)
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
    s->level = linear_to_perceptual(vols[0]);
    s->muted = muted;
    s->n_channels = n_vals;

    finish(s);
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = on_node_param,
};

/* ------------------------------------------------------------------ */
/* Registry events                                                      */
/* ------------------------------------------------------------------ */

static void on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                      uint32_t version, const struct spa_dict *props) {
    PwState *s = data;
    (void)permissions;
    (void)version;

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && strcmp(name, "default") == 0 && !s->metadata) {
            s->metadata = pw_registry_bind(s->registry, id, PW_TYPE_INTERFACE_Metadata,
                                           PW_VERSION_METADATA, 0);
            pw_metadata_add_listener(s->metadata, &s->metadata_listener, &metadata_events, s);
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

        if (s->n_sinks < MAX_SINKS) {
            const char *dev_str = spa_dict_lookup(props, "device.id");
            s->sinks[s->n_sinks].id = id;
            s->sinks[s->n_sinks].device_id = dev_str ? (uint32_t)strtoul(dev_str, NULL, 10) : 0;
            snprintf(s->sinks[s->n_sinks].name, sizeof(s->sinks[s->n_sinks].name), "%s", node_name);
            s->n_sinks++;
        }
    }
}

static void on_global_remove(void *data, uint32_t id) {
    PwState *s = data;
    if (id == s->target_id) {
        fprintf(stderr, "nyq: default sink removed\n");
        s->error = -1;
        pw_main_loop_quit(s->loop);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

/* ------------------------------------------------------------------ */
/* Phase transitions                                                    */
/* ------------------------------------------------------------------ */

static void start_bind(PwState *s) {
    if (!s->metadata) {
        fprintf(stderr, "nyq: no default metadata found\n");
        s->error = -1;
        pw_main_loop_quit(s->loop);
        return;
    }

    /* sink cycle commands don't need a node */
    if (s->cmd == PW_CMD_SINK_NEXT || s->cmd == PW_CMD_SINK_PREV) {
        finish(s);
        return;
    }

    /* resolve target_id from target_name */
    s->target_id = 0;
    for (int i = 0; i < s->n_sinks; i++) {
        if (strcmp(s->sinks[i].name, s->target_name) == 0) {
            s->target_id = s->sinks[i].id;
            break;
        }
    }
    /* fallback: first sink */
    if (s->target_id == 0 && s->n_sinks > 0)
        s->target_id = s->sinks[0].id;

    if (s->target_id == 0) {
        fprintf(stderr, "nyq: no audio sink found\n");
        s->error = -1;
        pw_main_loop_quit(s->loop);
        return;
    }

    s->node =
        pw_registry_bind(s->registry, s->target_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    pw_node_add_listener(s->node, &s->node_listener, &node_events, s);

    /* bind parent device if available */
    s->device = NULL;
    s->card_device = -1;
    s->route_index = -1;
    for (int i = 0; i < s->n_sinks; i++) {
        if (s->sinks[i].id == s->target_id && s->sinks[i].device_id > 0) {
            s->device = (struct pw_device *)pw_registry_bind(
                s->registry, s->sinks[i].device_id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
            if (s->device)
                pw_device_add_listener(s->device, &s->device_listener, &device_events, s);
            break;
        }
    }

    s->sync_seq = pw_core_sync(s->core, PW_ID_CORE, 0);
}

static void start_routes(PwState *s) {
    if (s->device) {
        pw_device_enum_params(s->device, 0, SPA_PARAM_Route, 0, -1, NULL);
    }
    s->sync_seq = pw_core_sync(s->core, PW_ID_CORE, 0);
}

static void start_params(PwState *s) {
    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(s->node, ids, 1);
    pw_node_enum_params(s->node, 0, SPA_PARAM_Props, 0, 1, NULL);
    s->sync_seq = pw_core_sync(s->core, PW_ID_CORE, 0);
}

/* ------------------------------------------------------------------ */
/* Sink cycling                                                          */
/* ------------------------------------------------------------------ */

static void do_sink_cycle(PwState *s, int dir) {
    if (s->n_sinks == 0) {
        fprintf(stderr, "nyq: no sinks available\n");
        s->error = -1;
        pw_main_loop_quit(s->loop);
        return;
    }

    /* find current default in sink list */
    int cur = 0;
    for (int i = 0; i < s->n_sinks; i++) {
        if (strcmp(s->sinks[i].name, s->target_name) == 0) {
            cur = i;
            break;
        }
    }

    int next = (cur + dir + s->n_sinks) % s->n_sinks;
    const char *new_name = s->sinks[next].name;

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "name", new_name);
    char *value = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (value) {
        pw_metadata_set_property(s->metadata, PW_ID_CORE, "default.audio.sink", "Spa:String:JSON",
                                 value);
        pw_metadata_set_property(s->metadata, PW_ID_CORE, "default.configured.audio.sink",
                                 "Spa:String:JSON", value);
        cJSON_free(value);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sink-switch");
    cJSON_AddStringToObject(root, "name", new_name);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out) {
        fprintf(stdout, "%s\n", out);
        fflush(stdout);
        cJSON_free(out);
    }

    s->phase = PHASE_DONE;
    pw_main_loop_quit(s->loop);
}

/* ------------------------------------------------------------------ */
/* Route set helper — mirrors pulse-server exactly                      */
/* ------------------------------------------------------------------ */

static void set_via_route(PwState *s, const float *vols, uint32_t n_ch, bool mute, bool set_vol,
                          bool set_mute) {
    char buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_pod_frame f[2];

    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_add(&b, SPA_PARAM_ROUTE_index, SPA_POD_Int(s->route_index),
                        SPA_PARAM_ROUTE_device, SPA_POD_Int(s->card_device), 0);

    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
    spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

    if (set_vol && vols && n_ch > 0)
        spa_pod_builder_add(&b, SPA_PROP_channelVolumes,
                            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_ch, vols), 0);
    if (set_mute)
        spa_pod_builder_add(&b, SPA_PROP_mute, SPA_POD_Bool(mute), 0);

    spa_pod_builder_pop(&b, &f[1]);

    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool(&b, true);

    struct spa_pod *param = spa_pod_builder_pop(&b, &f[0]);
    pw_device_set_param(s->device, SPA_PARAM_Route, 0, param);
}

/* ------------------------------------------------------------------ */
/* Finish                                                               */
/* ------------------------------------------------------------------ */

static void finish(PwState *s) {
    if (s->cmd == PW_CMD_SINK_NEXT) {
        do_sink_cycle(s, +1);
        return;
    }
    if (s->cmd == PW_CMD_SINK_PREV) {
        do_sink_cycle(s, -1);
        return;
    }

    bool via_route = (s->device != NULL && s->card_device >= 0 && s->route_index >= 0);

    if (s->cmd == PW_CMD_VOL_UP || s->cmd == PW_CMD_VOL_DOWN) {
        int delta = (s->cmd == PW_CMD_VOL_UP) ? +5 : -5;
        float new_level = volume_step(s->level, delta);
        float new_linear = perceptual_to_linear(new_level);

        float vols[8];
        for (uint32_t i = 0; i < s->n_channels; i++)
            vols[i] = new_linear;

        if (via_route) {
            set_via_route(s, vols, s->n_channels, false, true, false);
        } else {
            uint8_t buf[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
            struct spa_pod *param = spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_channelVolumes,
                SPA_POD_Array(sizeof(float), SPA_TYPE_Float, s->n_channels, vols));
            pw_node_set_param(s->node, SPA_PARAM_Props, 0, param);
        }
        s->level = new_level;

    } else if (s->cmd == PW_CMD_MUTE) {
        bool new_mute = !s->muted;
        if (via_route) {
            set_via_route(s, NULL, 0, new_mute, false, true);
        } else {
            uint8_t buf[256];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
            struct spa_pod *param = spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(new_mute));
            pw_node_set_param(s->node, SPA_PARAM_Props, 0, param);
        }
        s->muted = new_mute;
    }

    emit_sink(STDOUT_FILENO, s->level, s->muted);
    s->phase = PHASE_DONE;
    pw_main_loop_quit(s->loop);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

static int pw_oneshot_run(PwCmd cmd) {
    PwState s = {0};
    s.cmd = cmd;
    s.card_device = -1;
    s.route_index = -1;

    pw_init(NULL, NULL);

    s.loop = pw_main_loop_new(NULL);
    if (!s.loop) {
        fprintf(stderr, "nyq: pw_main_loop_new failed\n");
        goto cleanup;
    }

    s.ctx = pw_context_new(pw_main_loop_get_loop(s.loop), NULL, 0);
    if (!s.ctx) {
        fprintf(stderr, "nyq: pw_context_new failed\n");
        goto cleanup;
    }

    s.core = pw_context_connect(s.ctx, NULL, 0);
    if (!s.core) {
        fprintf(stderr, "nyq: pw_context_connect failed\n");
        goto cleanup;
    }

    s.registry = pw_core_get_registry(s.core, PW_VERSION_REGISTRY, 0);
    pw_core_add_listener(s.core, &s.core_listener, &core_events, &s);
    pw_registry_add_listener(s.registry, &s.registry_listener, &registry_events, &s);

    s.phase = PHASE_WAIT_GLOBALS;
    s.sync_seq = pw_core_sync(s.core, PW_ID_CORE, 0);

    pw_main_loop_run(s.loop);

cleanup:
    if (s.node) {
        spa_hook_remove(&s.node_listener);
        pw_proxy_destroy((struct pw_proxy *)s.node);
    }
    if (s.device) {
        spa_hook_remove(&s.device_listener);
        pw_proxy_destroy((struct pw_proxy *)s.device);
    }
    if (s.metadata) {
        spa_hook_remove(&s.metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)s.metadata);
    }
    if (s.registry) {
        spa_hook_remove(&s.registry_listener);
        pw_proxy_destroy((struct pw_proxy *)s.registry);
    }
    if (s.core)
        pw_core_disconnect(s.core);
    if (s.ctx)
        pw_context_destroy(s.ctx);
    if (s.loop)
        pw_main_loop_destroy(s.loop);
    pw_deinit();

    return s.error;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int pw_oneshot_status(void) {
    return pw_oneshot_run(PW_CMD_STATUS);
}
int pw_oneshot_vol_up(void) {
    return pw_oneshot_run(PW_CMD_VOL_UP);
}
int pw_oneshot_vol_down(void) {
    return pw_oneshot_run(PW_CMD_VOL_DOWN);
}
int pw_oneshot_mute(void) {
    return pw_oneshot_run(PW_CMD_MUTE);
}
int pw_oneshot_sink_next(void) {
    return pw_oneshot_run(PW_CMD_SINK_NEXT);
}
int pw_oneshot_sink_prev(void) {
    return pw_oneshot_run(PW_CMD_SINK_PREV);
}
