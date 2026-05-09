#pragma once

#include <stdbool.h>

/* One-shot sink operations. All block until complete then return.
 * Return 0 on success, -1 on error. */

int pw_oneshot_status(void);   /* print sink JSON to stdout */
int pw_oneshot_vol_up(void);   /* volume +5% */
int pw_oneshot_vol_down(void); /* volume -5% */
int pw_oneshot_mute(void);     /* toggle mute */
int pw_oneshot_sink_next(void);
int pw_oneshot_sink_prev(void);
