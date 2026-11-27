/*
 * suite_netgate.h - one background network fetch at a time, and never one
 * while Retro Web is loading a page.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop (Navigator 1.8), Phase 2A,
 * docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md.
 *
 * WHY. Retro Web parses HTML through libwww, which keeps its parse in a
 * process-global and is single-flight by contract: web_module.c refuses a
 * second fetch while g_b_fetch is set, and the May desktop's synchronous
 * helpers deferred to it the same way. The Teletext Desktop adds
 * background fetchers (the channel store now; desktop items next). They
 * share this one gate:
 *
 *   - Retro Web has priority and NEVER waits: it does not take the gate.
 *     Its loads are seen through a probe, web_fetch_in_flight() in
 *     web_module.c, which the shell registers with netgate_set_web_probe.
 *   - a background fetcher calls netgate_acquire before each request; it
 *     waits while Retro Web is loading or another background fetcher holds
 *     the gate, and gives up when its cancel flag is raised or the wait
 *     runs out;
 *   - a page load that starts while a background request is in flight is
 *     not delayed. That is safe because the channel store does not use
 *     libwww (it uses WinHTTP); a desktop item that parses HTML through
 *     libwww must also re-check netgate_web_busy() on its UI-thread side
 *     before handing work to the parser.
 */
#ifndef SUITE_NETGATE_H
#define SUITE_NETGATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the "is Retro Web loading?" probe (any thread may call it;
 * it must be cheap and lock-free). NULL = never busy. */
void netgate_set_web_probe(int (*probe)(void));

/* Retro Web is loading a page right now. */
BOOL netgate_web_busy(void);

/* Take the gate. Waits (polling every 100 ms) while Retro Web is busy or
 * another fetcher holds it. Returns TRUE when held; FALSE if *cancel
 * became non-zero or max_wait_ms passed (INFINITE = no limit). */
BOOL netgate_acquire(volatile LONG *cancel, DWORD max_wait_ms);

/* Give it back. Only the holder may call this. */
void netgate_release(void);

#ifdef __cplusplus
}
#endif

#endif /* SUITE_NETGATE_H */
