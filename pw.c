#include "pw.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

/* ------------------------------------------------------------------ */
/* State shared across callbacks                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PHASE_WAIT_GLOBALS,   /* waiting for initial registry burst */
    PHASE_WAIT_BIND,      /* waiting for metadata + node to bind */
    PHASE_WAIT_PARAMS,    /* waiting for Props param */
    PHASE_DONE,
} Phase;

typedef struct {
    struct pw_main_loop    *loop;
    struct pw_context      *ctx;
    struct pw_core         *core;
    struct pw_registry     *registry;

    struct spa_hook         core_listener;
    struct spa_hook         registry_listener;
    struct spa_hook         metadata_listener;
    struct spa_hook         node_listener;

    struct pw_metadata     *metadata;
    struct pw_node         *node;

    /* node id we are targeting */
    uint32_t                target_id;
    char                    target_name[256];

    /* current state read from Props */
    float                   level;      /* perceptual 0.0-1.0 */
    bool                    muted;
    uint32_t                n_channels;

    /* what to do after reading current state */
    int                     delta;      /* +5 / -5 / 0 for status */
    bool                    do_mute_toggle;

    int                     sync_seq;
    Phase                   phase;
    int                     error;
} PwState;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void start_bind(PwState *s);
static void start_params(PwState *s);
static void finish(PwState *s);

/* ------------------------------------------------------------------ */
/* Core events                                                          */
/* ------------------------------------------------------------------ */

static void on_core_done(void *data, uint32_t id, int seq) {
    PwState *s = data;
    if (id != PW_ID_CORE || seq != s->sync_seq) return;

    switch (s->phase) {
    case PHASE_WAIT_GLOBALS:
        s->phase = PHASE_WAIT_BIND;
        start_bind(s);
        break;
    case PHASE_WAIT_BIND:
        s->phase = PHASE_WAIT_PARAMS;
        start_params(s);
        break;
    default:
        break;
    }
}

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message) {
    PwState *s = data;
    fprintf(stderr, "nyq: pipewire error id=%u seq=%d res=%d: %s\n",
            id, seq, res, message);
    s->error = res;
    pw_main_loop_quit(s->loop);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = on_core_done,
    .error = on_core_error,
};

/* ------------------------------------------------------------------ */
/* Metadata events                                                      */
/* ------------------------------------------------------------------ */

static int on_metadata_property(void *data, uint32_t subject,
                                const char *key, const char *type,
                                const char *value) {
    PwState *s = data;
    (void)type;

    if (subject != PW_ID_CORE) return 0;
    if (!key || !value)        return 0;
    if (strcmp(key, "default.audio.sink") != 0) return 0;

    if (parse_name_json(value, s->target_name, sizeof(s->target_name)) < 0) {
        fprintf(stderr, "nyq: failed to parse default sink name from: %s\n",
                value);
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

/* ------------------------------------------------------------------ */
/* Node param event                                                     */
/* ------------------------------------------------------------------ */

static void on_node_param(void *data, int seq, uint32_t id,
                          uint32_t index, uint32_t next,
                          const struct spa_pod *param) {
    PwState *s = data;
    (void)seq; (void)index; (void)next;

    if (id != SPA_PARAM_Props) return;

    uint32_t n_vals = 0, val_size = 0, val_type = 0;
    const void *arr_body = NULL;
    bool muted = false;

    spa_pod_parse_object(param,
        SPA_TYPE_OBJECT_Props, NULL,
        SPA_PROP_channelVolumes, SPA_POD_OPT_Array(&val_size, &val_type,
                                                   &n_vals, &arr_body),
        SPA_PROP_mute,          SPA_POD_OPT_Bool(&muted));

    if (!arr_body || val_type != SPA_TYPE_Float || n_vals == 0) return;

    float *vols = (float *)arr_body;
    s->level      = linear_to_perceptual(vols[0]);
    s->muted      = muted;
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

static void on_global(void *data, uint32_t id, uint32_t permissions,
                      const char *type, uint32_t version,
                      const struct spa_dict *props) {
    PwState *s = data;
    (void)permissions; (void)version;

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && strcmp(name, "default") == 0 && !s->metadata) {
            s->metadata = pw_registry_bind(s->registry, id,
                              PW_TYPE_INTERFACE_Metadata,
                              PW_VERSION_METADATA, 0);
            pw_metadata_add_listener(s->metadata, &s->metadata_listener,
                                     &metadata_events, s);
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *node_name   = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!media_class || !node_name) return;
        if (strcmp(media_class, "Audio/Sink") != 0) return;

        /* store all sink candidates — we pick the right one in start_bind
         * once we know target_name from metadata */
        if (s->target_id == 0 ||
            (s->target_name[0] != '\0' &&
             strcmp(node_name, s->target_name) == 0)) {
            s->target_id = id;
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
    .global        = on_global,
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
    if (s->target_id == 0) {
        fprintf(stderr, "nyq: no audio sink found\n");
        s->error = -1;
        pw_main_loop_quit(s->loop);
        return;
    }

    /* bind the node we identified */
    s->node = pw_registry_bind(s->registry, s->target_id,
                  PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
                  sizeof(PwState *));
    pw_node_add_listener(s->node, &s->node_listener, &node_events, s);

    /* sync again to wait for the bind to complete */
    s->sync_seq = pw_core_sync(s->core, PW_ID_CORE, 0);
}

static void start_params(PwState *s) {
    /* enum Props — will trigger on_node_param */
    uint32_t ids[] = { SPA_PARAM_Props };
    pw_node_subscribe_params(s->node, ids, 1);
    pw_node_enum_params(s->node, 0, SPA_PARAM_Props, 0, 1, NULL);
}

static void finish(PwState *s) {
    if (s->delta != 0) {
        /* raise or lower */
        float new_level = volume_step(s->level, s->delta);
        float new_linear = perceptual_to_linear(new_level);

        uint8_t buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        float vols[8];
        for (uint32_t i = 0; i < s->n_channels; i++)
            vols[i] = new_linear;

        struct spa_pod *param = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
                                                   SPA_TYPE_Float,
                                                   s->n_channels, vols));
        pw_node_set_param(s->node, SPA_PARAM_Props, 0, param);
        s->level = new_level;

    } else if (s->do_mute_toggle) {
        bool new_mute = !s->muted;
        uint8_t buf[256];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        struct spa_pod *param = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_mute, SPA_POD_Bool(new_mute));
        pw_node_set_param(s->node, SPA_PARAM_Props, 0, param);
        s->muted = new_mute;
    }

    emit_sink(STDOUT_FILENO, s->level, s->muted);
    s->phase = PHASE_DONE;
    pw_main_loop_quit(s->loop);
}

/* ------------------------------------------------------------------ */
/* Common entry point                                                   */
/* ------------------------------------------------------------------ */

static int pw_oneshot_run(int delta, bool do_mute_toggle) {
    PwState s = {0};
    s.delta          = delta;
    s.do_mute_toggle = do_mute_toggle;

    pw_init(NULL, NULL);

    s.loop = pw_main_loop_new(NULL);
    if (!s.loop) { fprintf(stderr, "nyq: pw_main_loop_new failed\n"); return -1; }

    s.ctx = pw_context_new(pw_main_loop_get_loop(s.loop), NULL, 0);
    if (!s.ctx) { fprintf(stderr, "nyq: pw_context_new failed\n"); return -1; }

    s.core = pw_context_connect(s.ctx, NULL, 0);
    if (!s.core) { fprintf(stderr, "nyq: pw_context_connect failed\n"); return -1; }

    s.registry = pw_core_get_registry(s.core, PW_VERSION_REGISTRY, 0);
    pw_core_add_listener(s.core, &s.core_listener, &core_events, &s);
    pw_registry_add_listener(s.registry, &s.registry_listener,
                             &registry_events, &s);

    /* first sync: wait for initial globals burst */
    s.phase    = PHASE_WAIT_GLOBALS;
    s.sync_seq = pw_core_sync(s.core, PW_ID_CORE, 0);

    pw_main_loop_run(s.loop);

    /* cleanup */
    if (s.node)     { pw_proxy_destroy((struct pw_proxy *)s.node); }
    if (s.metadata) { pw_proxy_destroy((struct pw_proxy *)s.metadata); }
    pw_proxy_destroy((struct pw_proxy *)s.registry);
    pw_core_disconnect(s.core);
    pw_context_destroy(s.ctx);
    pw_main_loop_destroy(s.loop);
    pw_deinit();

    return s.error;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int pw_oneshot_status(void) { return pw_oneshot_run(0, false); }
int pw_oneshot_raise(void)  { return pw_oneshot_run(+5, false); }
int pw_oneshot_lower(void)  { return pw_oneshot_run(-5, false); }
int pw_oneshot_mute(void)   { return pw_oneshot_run(0, true); }
