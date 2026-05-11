#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <systemd/sd-journal.h>
#include <syslog.h>

#define log_err(fmt, ...)                                                                          \
    do {                                                                                           \
        fprintf(stderr, "nyq: " fmt "\n", ##__VA_ARGS__);                                          \
        sd_journal_print(LOG_ERR, fmt, ##__VA_ARGS__);                                             \
    } while (0)

#define log_warn(fmt, ...)                                                                         \
    do {                                                                                           \
        fprintf(stderr, "nyq: " fmt "\n", ##__VA_ARGS__);                                          \
        sd_journal_print(LOG_WARNING, fmt, ##__VA_ARGS__);                                         \
    } while (0)

#define log_info(fmt, ...)                                                                         \
    do {                                                                                           \
        fprintf(stderr, "nyq: " fmt "\n", ##__VA_ARGS__);                                          \
        sd_journal_print(LOG_INFO, fmt, ##__VA_ARGS__);                                            \
    } while (0)

/* Volume math
 * PipeWire wire format: linear (0.0–1.0)
 * Display/user-facing: perceptual (cubic root)
 */
float linear_to_perceptual(float linear);     /* cbrtf */
float perceptual_to_linear(float perceptual); /* v^3  */

/* Snap level (0.0–1.0 perceptual) to nearest 5% step, apply delta (+5 or -5) */
float volume_step(float level, int delta);

/* Icon name for current volume state */
const char *volume_icon(float level, bool muted);

/* Parse {"name":"..."} -> writes into buf (max len), returns 0 on success */
int parse_name_json(const char *json, char *buf, int len);

/* Emit sink event JSON to fd (pass 1 for stdout) */
void emit_sink(int fd, float level, bool muted);

/* Emit player event JSON to fd */
void emit_player(int fd, const char *name, const char *title, const char *artist,
                 const char *status, double volume);

/* Bounded string copy: copies src into dst (dst_size bytes), always null-terminates.
 * Returns dst. No-op if dst_size is 0. */
static inline char *s_copy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0)
        return dst;
    size_t n = strlen(src);
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
}
