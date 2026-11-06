/*
 * nabu_serial.h - the serial leg: a real NABU on a cable, a channel
 *                 server on a socket, and nothing in between.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 *
 * WHAT THIS IS FOR.
 *
 * A real NABU Personal Computer has no network. It has an HCCA port, a
 * serial line that expected an adapter box on the end of a cable running
 * to the head end. Owners of real machines today run a separate Internet
 * Adapter program that opens that serial port and relays the adapter
 * protocol to a channel server over TCP. This is Navigator doing that
 * job, so Navigator becomes the missing serial leg rather than another
 * content server.
 *
 * THE SUITE ADDS NOTHING TO THE STREAM. Once the relay is running, every
 * byte off the cable goes to the socket and every byte off the socket
 * goes to the cable, in order, unexamined and unmodified. Navigator does
 * not know or care what the bytes mean, which is the whole point: the
 * real machine boots its own genuine ROM and talks to nabud exactly as
 * it would through any other adapter.
 *
 * The one thing sent that the NABU did not say is the channel selection,
 * and it goes out BEFORE the relay starts and only ever towards the
 * server, once per connection. The cable never sees it.
 *
 * STAYING UP UNATTENDED (2026-10-31, the NABU hang fix for VCF Montreal).
 * nabu_serial_run is the whole session: it opens the cable, connects,
 * relays, and when the network drops it reconnects by itself with a
 * backoff, redoing the channel selection, until it is told to stop. TCP
 * keepalive is on, so a path that dies silently (venue WiFi) is noticed
 * in about 25 s instead of never. See docs and
 * mgt-nabu\docs\2026-10-31_NABU_HANG_FIX.md.
 */

#ifndef NABU_SERIAL_H
#define NABU_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE NABU'S NATIVE LINE RATE.
 *
 * Not a round number and not a guess. The machine clocks its UART from
 * the NTSC colourburst crystal:
 *
 *     3.579545 MHz  /  2               /  16
 *     colourburst      on-board divider   on-chip divider on the TR1863
 *
 * which is 111860.625 bits per second. Verified against nabud's own
 * source, nabud/conn.c, which derives it the same way and defines
 * NABU_NATIVE_BPS as ((3579540 / 2) / 16). The integer value both ends
 * use is therefore 111860.
 *
 * NOTE that a widely repeated figure of 111865 is wrong; the derivation
 * above is what the hardware actually does.
 *
 * NABU_BRIDGE_TEST (a TEST-ONLY build, never installed or published)
 * runs the cable at 115200 instead, because the Z840's on-board COM1, the
 * only serial port the test bench has, refuses 111860 (error 87). That
 * build says so in its title bar.
 */
#ifdef NABU_BRIDGE_TEST
#define NABU_SERIAL_BAUD 115200
#else
#define NABU_SERIAL_BAUD 111860
#endif

/*
 * EIGHT DATA BITS, NO PARITY, TWO STOP BITS.
 *
 * The native protocol is 8N1. Two stop bits is deliberate and is what
 * nabud defaults to, in its own words because "it's much more reliable
 * if we use 2 stop bits. Otherwise, the NABU can get out of sync when
 * receiving a stream of bytes in a packet." The extra stop bit costs a
 * little throughput and buys the machine time to keep up, so Navigator
 * defaults the same way and for the same reason.
 */
#define NABU_SERIAL_STOPBITS 2

/*
 * KEEPALIVE: idle 10 s, then a probe every 3 s, 5 probes. A path that
 * stops answering is declared dead about 25 s after the last thing heard.
 * Data sent and never acknowledged is given up on after NABU_SERIAL_MAXRT
 * seconds, which keepalive does not cover.
 */
#define NABU_SERIAL_KA_IDLE_S   10
#define NABU_SERIAL_KA_INTVL_S   3
#define NABU_SERIAL_KA_COUNT     5
#define NABU_SERIAL_MAXRT_S     20

/* One line of the connection log, as the tab will show it. */
typedef void (*nabu_serial_log_fn)(void *user, const char *line);

typedef struct {
    const char *com_port;   /* "COM3", or a \\.\COM12 form for high numbers */
    const char *host;
    int         port;
    int         channel;    /* 0 to leave the server on its own default */
} nabu_serial_config_t;

/* Where the link is, for a display that must be readable from across a
 * table. */
typedef enum {
    NABU_LINK_CONNECTING = 1,   /* first connection, not yet made        */
    NABU_LINK_UP,               /* relaying                             */
    NABU_LINK_RECONNECTING,     /* was up, dropped, trying again         */
    NABU_LINK_STOPPED,          /* the caller asked it to stop           */
    NABU_LINK_FAILED            /* cannot start at all (the cable)       */
} nabu_link_state_t;

typedef struct {
    int       state;            /* nabu_link_state_t                     */
    int       attempt;          /* 1 based, this round of trying         */
    int       next_try_s;       /* seconds to the next attempt, 0 = now  */
    unsigned  reconnects;       /* successful reconnections this session */
    uint64_t  up_since_ms;      /* GetTickCount64 at the last connect    */
    char      why[160];         /* the last problem, for the display     */
} nabu_link_info_t;

/* Called on every change of the above, and once a second while trying
 * or relaying (so a countdown or an uptime can be shown). From the
 * session's own thread. */
typedef void (*nabu_serial_state_fn)(void *user, const nabu_link_info_t *info);

/*
 * THE WHOLE SESSION, until `stop_event` (a Win32 event HANDLE) is set.
 *
 * Opens the serial port FIRST: a wrong COM port or a rate the driver will
 * not take is the likely mistake, it will not cure itself, and it is
 * reported as FAILED at once with nothing left open (returns -1).
 *
 * Then the network, and the network is NOT expected to behave: a
 * connection that cannot be made, or that drops, or that keepalive
 * declares dead, is retried with a backoff of 1, 2, 4, 8 and then every
 * 15 s, for as long as it takes. Before every attempt, whatever the NABU
 * sent while the link was down is thrown away, so nabud never receives
 * half a request; the NABU's own bounded reads (the pak's 12 s silence
 * deadline) take care of the request that was in flight. The channel
 * selection is redone on every new connection. A serial port that
 * vanishes (a USB adapter unplugged) is reopened the same way.
 *
 * Returns 0 when stopped. Everything is closed on return.
 */
int nabu_serial_run(const nabu_serial_config_t *cfg, void *stop_event,
                    nabu_serial_log_fn log_fn,
                    nabu_serial_state_fn state_fn, void *user);

/* Bytes relayed this session, for the log. */
void nabu_serial_counts(uint64_t *to_nabu, uint64_t *to_server);

/*
 * The machine's serial ports, newest enumeration each call. Fills names
 * with up to max entries of the form "COM3" and returns how many.
 */
int nabu_serial_enumerate(char names[][16], int max);

#ifdef __cplusplus
}
#endif

#endif /* NABU_SERIAL_H */
