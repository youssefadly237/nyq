#pragma once

/* Start the nyq daemon. Blocks until SIGTERM or SIGINT.
 * Returns 0 on clean exit, -1 on error. */
int daemon_run(void);
