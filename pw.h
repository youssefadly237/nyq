#pragma once

#include <stdbool.h>

/* One-shot sink operations. All block until complete then return.
 * Return 0 on success, -1 on error. */

int pw_oneshot_status(void);        /* print sink JSON to stdout */
int pw_oneshot_raise(void);         /* volume +5% */
int pw_oneshot_lower(void);         /* volume -5% */
int pw_oneshot_mute(void);          /* toggle mute */
