#pragma once

#include <stdbool.h>
#include <stdint.h>

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
