#include "util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>

/* write() has warn_unused_result, (void) cast does not suppress it on gcc.
 * This wrapper silences it for best-effort fire-and-forget writes. */
static void write_all(int fd, const char *buf, int n) {
    if (n > 0) {
        int r = write(fd, buf, (size_t)n);
        (void)r;
    }
}

float linear_to_perceptual(float linear) {
    return cbrtf(linear);
}

float perceptual_to_linear(float perceptual) {
    return perceptual * perceptual * perceptual;
}

float volume_step(float level, int delta) {
    int pct = (int)roundf(level * 100.0f);

    if (pct % 5 == 0) {
        pct += delta;
    } else if (delta > 0) {
        pct = (pct / 5 + 1) * 5;
    } else {
        pct = (pct / 5) * 5;
    }

    if (pct > 100)
        pct = 100;
    if (pct < 0)
        pct = 0;

    return pct / 100.0f;
}

const char *volume_icon(float level, bool muted) {
    if (muted || level == 0.0f)
        return "";
    if (level < 0.33f)
        return "";
    if (level < 0.66f)
        return "";
    return "";
}

int parse_name_json(const char *json, char *buf, int len) {
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        cJSON_Delete(root);
        return -1;
    }

    s_copy(buf, len, name->valuestring);
    cJSON_Delete(root);
    return 0;
}

static void emit_json(int fd, cJSON *root) {
    char *str = cJSON_PrintUnformatted(root);
    if (!str)
        return;

    int n = strlen(str);
    write_all(fd, str, n);
    write_all(fd, "\n", 1);

    cJSON_free(str);
}

void emit_sink(int fd, float level, bool muted) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sink");
    cJSON_AddNumberToObject(root, "level", (double)level);
    cJSON_AddBoolToObject(root, "muted", muted);
    cJSON_AddStringToObject(root, "icon", volume_icon(level, muted));

    emit_json(fd, root);
    cJSON_Delete(root);
}

void emit_player(int fd, const char *name, const char *title, const char *artist,
                 const char *status, double volume) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "player");
    cJSON_AddStringToObject(root, "name", name ? name : "");
    cJSON_AddStringToObject(root, "title", title ? title : "");
    cJSON_AddStringToObject(root, "artist", artist ? artist : "");
    cJSON_AddStringToObject(root, "status", status ? status : "Stopped");
    cJSON_AddNumberToObject(root, "volume", volume);

    emit_json(fd, root);
    cJSON_Delete(root);
}
