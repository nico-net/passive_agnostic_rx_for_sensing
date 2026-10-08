/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SHUTDOWN_H
#define NR_SHUTDOWN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize before opening any radio or starting workers. Callbacks run on a
 * dedicated thread, never in a signal handler. One process, one shutdown. */
int nr_shutdown_init(void (*notify_main)(void), void (*stop_radio)(void), void (*release_radio)(void));
/* Async-signal-safe, idempotent, first request starts the deadline. */
void nr_shutdown_request(int exit_status);
void nr_shutdown_signal(int signal_number);
int nr_shutdown_requested(void);
int nr_shutdown_status(void);
void nr_shutdown_wait_radio(void);
void nr_shutdown_step(const char *step);
/* Device lifetime gate. Existing reads/writes finish before release; no new
 * calls or device reinitializations may enter after a shutdown request. */
int nr_shutdown_radio_enter(int exclusive);
void nr_shutdown_radio_leave(void);
/* Ends all remaining service threads without running late plugin destructors. */
#ifdef __cplusplus
[[noreturn]] void nr_shutdown_finish(void);
#else
_Noreturn void nr_shutdown_finish(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
