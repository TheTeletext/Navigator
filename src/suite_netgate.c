/*
 * suite_netgate.c - one background network fetch at a time, and never one
 * while Retro Web is loading a page. See suite_netgate.h.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#include "suite_netgate.h"

static volatile LONG g_held = 0;
static int (*volatile g_probe)(void) = NULL;

void netgate_set_web_probe(int (*probe)(void))
{
    g_probe = probe;
}

BOOL netgate_web_busy(void)
{
    int (*p)(void) = g_probe;
    return p ? (p() != 0) : FALSE;
}

BOOL netgate_acquire(volatile LONG *cancel, DWORD max_wait_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        if (cancel && InterlockedCompareExchange(cancel, 0, 0) != 0) return FALSE;
        if (!netgate_web_busy() && InterlockedCompareExchange(&g_held, 1, 0) == 0) {
            /* Retro Web may have started between the probe and the take:
             * look once more, and step back if so. */
            if (!netgate_web_busy()) return TRUE;
            InterlockedExchange(&g_held, 0);
        }
        if (max_wait_ms != INFINITE && GetTickCount() - start >= max_wait_ms) return FALSE;
        Sleep(100);
    }
}

void netgate_release(void)
{
    InterlockedExchange(&g_held, 0);
}
