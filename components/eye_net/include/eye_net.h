#pragma once
/*
 * eye_net — WiFi, structured so a network that changes cannot make the
 * mechanism unreachable.
 *
 * The build this replaces compiled a single SSID in from secrets.h, ran
 * station-only, and retried it forever. Change networks and you reflashed;
 * fail to associate and there was no way in at all, on a device whose only
 * other control surface was the network that just failed.
 *
 * Ported from esp/fab26-fubar-bot-demo/main/wifi_sta.c, which solves exactly
 * this for a demo bot at venues whose networks it does not control. The
 * invariants below are that project's, learned the hard way, and are the
 * reason this is shaped the way it is:
 *
 *   1. The radio is WIFI_MODE_APSTA from start to finish. The access point
 *      never goes down in any station state, so there is always a way in.
 *      Nothing calls esp_wifi_set_mode() again; station transitions
 *      reconfigure WIFI_IF_STA only.
 *   2. Event callbacks never sleep, connect, or change mode. They queue work
 *      to the manager task and return.
 *   3. A profile's threshold.authmode follows its stored password
 *      (WIFI_AUTH_OPEN when blank). An unconditional WPA2 threshold silently
 *      refuses to associate with open networks.
 *   4. Switching profiles forces a disconnect; those self-induced disconnect
 *      events are drained rather than charged to the new profile's retries.
 *   5. Only credentials that actually reached IP_EVENT_STA_GOT_IP are written
 *      to NVS, both as the boot default and as a profile at all. A typo must
 *      not survive a power cycle.
 *
 * Credentials live in NVS (namespace "eyenet"), seeded once from Kconfig if
 * NVS is empty. Nothing is compiled in, and nothing is logged.
 *
 * mDNS advertises <hostname>.local on both interfaces, so the board is
 * reachable by name whether it joined a network or you joined its AP.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EYE_NET_PROFILES  4
#define EYE_NET_SCAN_MAX  20

/* The AP is permanent, so these describe the station, not an exclusive radio
 * mode: AP_ONLY means no station link and the AP is the only path in. */
typedef enum {
    EYE_NET_STA_CONNECTED,
    EYE_NET_AP_ONLY,
    EYE_NET_CONNECTING,
} eye_net_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
} eye_net_scan_entry_t;

esp_err_t eye_net_start(void);

eye_net_state_t eye_net_state(void);
int  eye_net_ssid(char *buf, size_t len);     /* station SSID, or the AP's */
int  eye_net_ip(char *buf, size_t len);       /* station IP, or the AP's */
int  eye_net_ap_clients(void);
int  eye_net_active_profile(void);            /* last to reach GOT_IP, or -1 */
const char *eye_net_ap_ssid(void);

/* Profiles. Passwords are write-only from outside: there is no getter, and
 * they are never logged or returned over HTTP. */
int       eye_net_profile_ssid(int n, char *buf, size_t len);
esp_err_t eye_net_set_profile(int n, const char *ssid, const char *password);
esp_err_t eye_net_clear_profile(int n);
esp_err_t eye_net_connect_profile(int n);
esp_err_t eye_net_force_ap(void);             /* stop roaming, stay on the AP */
int       eye_net_oldest_profile(void);       /* next to be evicted, or -1 */

/* Joining by name, which is how credentials normally arrive: the SSID and
 * password are tried live and written to a profile only once the station
 * reaches an IP. Nothing is stored on the way in, so a typo costs an attempt
 * rather than a slot, and invariant 5 above holds by construction.
 *
 * The four profiles are a FIFO. A new network takes a free slot if there is
 * one and evicts the least recently joined if there is not, so joining never
 * asks which slot to use. Re-joining a network already on the list refreshes
 * that entry in place instead of consuming a second slot.
 *
 * The attempt runs on the manager task: call join, then poll join_result. */
typedef enum {
    EYE_NET_JOIN_IDLE,
    EYE_NET_JOIN_BUSY,
    EYE_NET_JOIN_OK,
    EYE_NET_JOIN_FAILED,
} eye_net_join_state_t;

esp_err_t eye_net_join(const char *ssid, const char *password);
/* Writes the profile the join landed in through *slot when it succeeded. */
eye_net_join_state_t eye_net_join_result(int *slot);

/* Scan runs on the manager task: start it, poll busy, then copy results. */
esp_err_t eye_net_scan_start(void);
bool      eye_net_scan_busy(void);
int       eye_net_scan_results(eye_net_scan_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
