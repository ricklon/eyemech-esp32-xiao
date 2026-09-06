#include "eye_net.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "eye_net";

/* Kconfig may not be wired up under every build front-end; keep the component
 * compilable either way rather than failing on an undefined symbol. */
#ifndef CONFIG_EYE_NET_STA_SSID
#  define CONFIG_EYE_NET_STA_SSID     ""
#endif
#ifndef CONFIG_EYE_NET_STA_PASSWORD
#  define CONFIG_EYE_NET_STA_PASSWORD ""
#endif
#ifndef CONFIG_EYE_NET_AP_SSID
#  define CONFIG_EYE_NET_AP_SSID      "eyemech-setup"
#endif
#ifndef CONFIG_EYE_NET_AP_PASSWORD
#  define CONFIG_EYE_NET_AP_PASSWORD  "eyemech123"
#endif
#ifndef CONFIG_EYE_NET_HOSTNAME
#  define CONFIG_EYE_NET_HOSTNAME     "eyemech"
#endif

#define AP_CHANNEL      6
#define AP_MAX_CONN     4
/* Not 192.168.4.1. That is IDF's default and also one of the most common home
 * router subnets — and this radio is APSTA at all times, so the station can be
 * on a LAN that overlaps the access point the recovery page lives on. Seen on
 * the bench: a router handing out 192.168.4.0/22 with gateway 192.168.4.1,
 * exactly the AP's own address. */
#define AP_IP_ADDR      "192.168.9.1"
#define AP_NETMASK      "255.255.255.0"
#define STA_MAX_RETRIES 3
/* Idle time on the AP before sweeping the profile list again, so a network
 * that drops out for a few minutes is picked back up unattended. */
#define AP_RESWEEP_MS   (120 * 1000)

#define NVS_NS      "eyenet"
#define NVS_ACTIVE  "active"

static const char *const s_ssid_keys[EYE_NET_PROFILES] = { "ssid0","ssid1","ssid2","ssid3" };
static const char *const s_pass_keys[EYE_NET_PROFILES] = { "pass0","pass1","pass2","pass3" };
/* Join order, so the FIFO knows which entry is the oldest. 0 means unset. */
static const char *const s_seq_keys[EYE_NET_PROFILES]  = { "seq0","seq1","seq2","seq3" };

/* s_trying holds a profile index, or one of these. */
#define PROFILE_NONE      (-1)
#define PROFILE_CANDIDATE (-2)   /* credentials being tried but not yet stored */

typedef enum {
    CMD_STA_START, CMD_GOT_IP, CMD_DISCONNECTED,
    CMD_CONNECT, CMD_START_AP, CMD_SCAN, CMD_JOIN,
} cmd_type_t;

typedef struct {
    cmd_type_t     type;
    esp_ip4_addr_t ip;
    int            profile;
} cmd_t;

static QueueHandle_t     s_q;
static SemaphoreHandle_t s_lock;
static esp_netif_t      *s_sta_netif, *s_ap_netif;

static eye_net_state_t s_state = EYE_NET_AP_ONLY;
static char s_ssid[33], s_ip[16];
static char s_prof_ssid[EYE_NET_PROFILES][33];
static char s_prof_pass[EYE_NET_PROFILES][65];
static uint32_t s_prof_seq[EYE_NET_PROFILES];   /* join order; 0 = never joined */
static uint32_t s_seq_next = 1;
static int  s_active = -1;      /* last profile to reach GOT_IP */
static int  s_trying = -1;      /* profile being attempted; see PROFILE_* */
static int  s_tried, s_retries;
static bool s_sweep_idle;

/* The candidate: credentials handed to eye_net_join() that have not earned a
 * profile slot yet. Cleared whether they work or not. */
static char s_cand_ssid[33], s_cand_pass[65];
static eye_net_join_state_t s_join = EYE_NET_JOIN_IDLE;
static int  s_join_slot = -1;

static eye_net_scan_entry_t s_scan[EYE_NET_SCAN_MAX];
static int  s_scan_count;
static bool s_scan_busy;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int configured_count(void)
{
    int n = 0;
    for (int i = 0; i < EYE_NET_PROFILES; i++) if (s_prof_ssid[i][0]) n++;
    return n;
}

static int next_configured(int after)
{
    for (int i = 1; i <= EYE_NET_PROFILES; i++) {
        int c = (after + i) % EYE_NET_PROFILES;
        if (s_prof_ssid[c][0]) return c;
    }
    return -1;
}

/* --------------------------------------------------------------- storage */

static void load_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < EYE_NET_PROFILES; i++) {
            size_t sl = sizeof(s_prof_ssid[i]), pl = sizeof(s_prof_pass[i]);
            if (nvs_get_str(h, s_ssid_keys[i], s_prof_ssid[i], &sl) != ESP_OK) s_prof_ssid[i][0] = '\0';
            if (nvs_get_str(h, s_pass_keys[i], s_prof_pass[i], &pl) != ESP_OK) s_prof_pass[i][0] = '\0';
            if (nvs_get_u32(h, s_seq_keys[i], &s_prof_seq[i]) != ESP_OK) s_prof_seq[i] = 0;
        }
        uint8_t a = 0;
        if (nvs_get_u8(h, NVS_ACTIVE, &a) == ESP_OK && a < EYE_NET_PROFILES && s_prof_ssid[a][0]) {
            s_active = a;
        }
        nvs_close(h);
    }

    /* Seed slot 0 from Kconfig only when NVS has nothing. NVS wins afterwards,
     * so changing networks never needs a reflash. */
    if (!s_prof_ssid[0][0] && CONFIG_EYE_NET_STA_SSID[0]) {
        strlcpy(s_prof_ssid[0], CONFIG_EYE_NET_STA_SSID, sizeof(s_prof_ssid[0]));
        strlcpy(s_prof_pass[0], CONFIG_EYE_NET_STA_PASSWORD, sizeof(s_prof_pass[0]));
        ESP_LOGI(TAG, "seeded profile 1 from build config");
    }
    if (s_active < 0) s_active = next_configured(-1);

    /* Profiles written before the FIFO existed, or by !wifi set <n>, carry no
     * join order. Give them one in slot order so eviction is defined, and
     * start new joins above the highest number in use. */
    for (int i = 0; i < EYE_NET_PROFILES; i++) {
        if (s_prof_ssid[i][0] && s_prof_seq[i] == 0) s_prof_seq[i] = s_seq_next++;
        if (s_prof_seq[i] >= s_seq_next) s_seq_next = s_prof_seq[i] + 1;
    }
}

/* The slot a newly joined network should take: the one already holding this
 * SSID, else a free one, else the least recently joined. */
static int fifo_slot_for(const char *ssid)
{
    for (int i = 0; i < EYE_NET_PROFILES; i++) {
        if (s_prof_ssid[i][0] && strcmp(s_prof_ssid[i], ssid) == 0) return i;
    }
    for (int i = 0; i < EYE_NET_PROFILES; i++) {
        if (!s_prof_ssid[i][0]) return i;
    }
    int oldest = 0;
    for (int i = 1; i < EYE_NET_PROFILES; i++) {
        if (s_prof_seq[i] < s_prof_seq[oldest]) oldest = i;
    }
    return oldest;
}

static void persist_active(int profile)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_ACTIVE, (uint8_t)profile);
    nvs_commit(h);
    nvs_close(h);
}

/* One writer for both paths into the profile list, so a manual set and a
 * successful join produce identical on-flash state. Every write is "just
 * joined" for FIFO purposes and takes the next sequence number. */
static esp_err_t write_profile(int n, const char *ssid, const char *password)
{
    lock();
    uint32_t seq = s_seq_next++;
    unlock();

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    esp_err_t err = nvs_set_str(h, s_ssid_keys[n], ssid);
    if (err == ESP_OK) err = nvs_set_str(h, s_pass_keys[n], password ? password : "");
    if (err == ESP_OK) err = nvs_set_u32(h, s_seq_keys[n], seq);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    lock();
    strlcpy(s_prof_ssid[n], ssid, sizeof(s_prof_ssid[n]));
    strlcpy(s_prof_pass[n], password ? password : "", sizeof(s_prof_pass[n]));
    s_prof_seq[n] = seq;
    unlock();
    return ESP_OK;
}

/* Called once, from GOT_IP, when the credentials being tried are a candidate.
 * Returns the slot it landed in, or -1 if the write failed. */
static int store_candidate(void)
{
    char ssid[sizeof(s_cand_ssid)], pass[sizeof(s_cand_pass)], evicted[sizeof(s_cand_ssid)];
    int n;

    lock();
    strlcpy(ssid, s_cand_ssid, sizeof(ssid));
    strlcpy(pass, s_cand_pass, sizeof(pass));
    n = fifo_slot_for(ssid);
    strlcpy(evicted, s_prof_ssid[n], sizeof(evicted));
    unlock();

    bool replacing = evicted[0] && strcmp(evicted, ssid) != 0;

    esp_err_t err = write_profile(n, ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "joined '%s' but could not store it: %s", ssid, esp_err_to_name(err));
        return -1;
    }
    /* Never the password, on either branch. */
    if (replacing) {
        ESP_LOGI(TAG, "profile %d: '%s' replaced '%s', the oldest entry", n + 1, ssid, evicted);
    } else {
        ESP_LOGI(TAG, "profile %d saved (SSID '%s')", n + 1, ssid);
    }
    return n;
}

/* ------------------------------------------------------------ transitions */

static void drain_self_induced_disconnects(void)
{
    cmd_t c;
    while (xQueueReceive(s_q, &c, 0) == pdTRUE) {
        if (c.type != CMD_DISCONNECTED) { (void)xQueueSendToFront(s_q, &c, 0); break; }
    }
}

static void enter_ap_only(bool allow_resweep)
{
    (void)esp_wifi_disconnect();
    lock();
    s_trying = PROFILE_NONE; s_tried = 0; s_retries = 0;
    s_sweep_idle = allow_resweep;
    if (s_join == EYE_NET_JOIN_BUSY) { s_join = EYE_NET_JOIN_FAILED; s_join_slot = -1; }
    s_cand_ssid[0] = '\0'; s_cand_pass[0] = '\0';
    strlcpy(s_ssid, CONFIG_EYE_NET_AP_SSID, sizeof(s_ssid));
    strlcpy(s_ip, AP_IP_ADDR, sizeof(s_ip));
    s_state = EYE_NET_AP_ONLY;
    unlock();
    ESP_LOGW(TAG, "no station link — join '%s' and browse http://%s/",
             CONFIG_EYE_NET_AP_SSID, AP_IP_ADDR);
}

/* The one place the station is pointed at a network. `profile` is the slot
 * these credentials belong to, or PROFILE_CANDIDATE while they are still
 * being auditioned. */
static esp_err_t sta_connect(const char *ssid, const char *password, int profile)
{
    wifi_config_t cfg = { 0 };
    lock();
    s_trying = profile; s_sweep_idle = false; s_retries = 0;
    s_state = EYE_NET_CONNECTING;
    s_ip[0] = '\0';
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    /* Open networks are common. An unconditional WPA2 threshold makes the
     * station silently refuse to associate with them. */
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    unlock();

    /* An SSID is often served by several access points; take the strongest
     * rather than the first answer. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Take the station down and wait until it is genuinely down before
     * touching the config. Both halves of this are asynchronous and both bite:
     *
     *   - while an association is still in flight, set_config is refused with
     *     "sta is connecting, cannot set config";
     *   - while the old link is still up, esp_wifi_connect() is ignored with
     *     "sta is connected, disconnect before connecting to new ap".
     *
     * The second is the dangerous one. The previous network stays connected,
     * its next IP event arrives, and credentials that were never tested look
     * like they worked — which for a join means a wrong password gets written
     * to a profile. A fixed delay is not enough; wait for the radio to agree. */
    (void)esp_wifi_disconnect();
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        drain_self_induced_disconnects();   /* not this attempt's fault */

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            (void)esp_wifi_disconnect();    /* still associated; ask again */
            continue;
        }
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err == ESP_OK) break;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "sta config");
    if (profile >= 0) ESP_LOGI(TAG, "trying profile %d (SSID '%s')", profile + 1, ssid);
    else              ESP_LOGI(TAG, "trying '%s'", ssid);
    return esp_wifi_connect();
}

static esp_err_t start_profile(int p)
{
    ESP_RETURN_ON_FALSE(p >= 0 && p < EYE_NET_PROFILES && s_prof_ssid[p][0],
                        ESP_ERR_INVALID_ARG, TAG, "profile %d not configured", p + 1);
    return sta_connect(s_prof_ssid[p], s_prof_pass[p], p);
}

static esp_err_t start_candidate(void)
{
    ESP_RETURN_ON_FALSE(s_cand_ssid[0], ESP_ERR_INVALID_ARG, TAG, "no candidate");
    return sta_connect(s_cand_ssid, s_cand_pass, PROFILE_CANDIDATE);
}

/* Back to whatever was working before an attempt that did not pan out. */
static void fall_back_to_known(void)
{
    int back = (s_active >= 0 && s_prof_ssid[s_active][0]) ? s_active : next_configured(-1);
    s_tried = 0;
    if (back < 0 || start_profile(back) != ESP_OK) enter_ap_only(true);
}

static void handle_disconnect(void)
{
    if (s_trying == PROFILE_NONE) return;   /* AP-only; nothing being attempted */

    lock();
    s_ip[0] = '\0';
    s_state = EYE_NET_CONNECTING;
    int retry = ++s_retries;
    unlock();

    if (retry <= STA_MAX_RETRIES) {
        if (s_trying == PROFILE_CANDIDATE) {
            ESP_LOGW(TAG, "'%s' did not associate, retry %d/%d",
                     s_cand_ssid, retry, STA_MAX_RETRIES);
        } else {
            ESP_LOGW(TAG, "profile %d disconnected, retry %d/%d",
                     s_trying + 1, retry, STA_MAX_RETRIES);
        }
        vTaskDelay(pdMS_TO_TICKS(500 * retry));
        (void)esp_wifi_connect();
        return;
    }

    /* A candidate out of retries is a wrong password or a network that is not
     * there. Nothing was written, so there is nothing to undo — drop it and
     * go back to a network that works rather than stranding the caller. */
    if (s_trying == PROFILE_CANDIDATE) {
        ESP_LOGW(TAG, "could not join '%s' — not saved", s_cand_ssid);
        lock();
        s_cand_ssid[0] = '\0'; s_cand_pass[0] = '\0';
        s_join = EYE_NET_JOIN_FAILED; s_join_slot = -1;
        unlock();
        fall_back_to_known();
        return;
    }

    /* Move along the list before giving up: that is what makes
     * home -> workshop -> phone-hotspot transitions happen unattended. */
    ++s_tried;
    int next = next_configured(s_trying);
    if (next < 0 || s_tried >= configured_count()) {
        enter_ap_only(true);
        return;
    }
    if (start_profile(next) != ESP_OK) enter_ap_only(true);
}

static void run_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        lock(); s_scan_busy = false; unlock();
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;

    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        lock(); s_scan_busy = false; unlock();
        return;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    lock();
    s_scan_count = 0;
    for (int i = 0; i < n && s_scan_count < EYE_NET_SCAN_MAX; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan[j].ssid, (char *)recs[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strlcpy(s_scan[s_scan_count].ssid, (char *)recs[i].ssid, sizeof(s_scan[0].ssid));
        s_scan[s_scan_count].rssi   = recs[i].rssi;
        s_scan[s_scan_count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        s_scan_count++;
    }
    s_scan_busy = false;
    unlock();
    free(recs);
    ESP_LOGI(TAG, "scan found %d networks", s_scan_count);
}

/* Advertise on both interfaces, so eyemech.local resolves whether the board is
 * on a station network or you have joined the recovery AP. Never fatal: mDNS
 * failing costs a convenience, and the numeric address still works. */
static void mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: %s — use the IP address",
                 esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_EYE_NET_HOSTNAME);
    mdns_instance_name_set("eyemech eye mechanism");
    /* Lets the control page be found by service browsing as well as by name. */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS up — http://%s.local/", CONFIG_EYE_NET_HOSTNAME);
}

/* ---------------------------------------------------------------- events */

static void queue_cmd(const cmd_t *c) { (void)xQueueSend(s_q, c, 0); }

/* Queues and returns. Never sleeps, connects, or changes mode here. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        const cmd_t c = { .type = CMD_STA_START }; queue_cmd(&c);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const cmd_t c = { .type = CMD_DISCONNECTED }; queue_cmd(&c);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        const cmd_t c = { .type = CMD_GOT_IP, .ip = e->ip_info.ip }; queue_cmd(&c);
    }
}

static void manager_task(void *arg)
{
    (void)arg;
    cmd_t cmd;

    for (;;) {
        TickType_t wait = (s_sweep_idle && configured_count() > 0)
                          ? pdMS_TO_TICKS(AP_RESWEEP_MS) : portMAX_DELAY;

        if (xQueueReceive(s_q, &cmd, wait) != pdTRUE) {
            int first = (s_active >= 0 && s_prof_ssid[s_active][0])
                        ? s_active : next_configured(-1);
            if (first >= 0) {
                ESP_LOGI(TAG, "re-checking station profiles");
                s_tried = 0;
                if (start_profile(first) != ESP_OK) enter_ap_only(true);
            }
            continue;
        }

        switch (cmd.type) {
        case CMD_STA_START:
            if (s_trying >= 0) (void)esp_wifi_connect();
            break;

        case CMD_GOT_IP: {
            /* Reaching an IP is the only thing that turns a candidate into a
             * stored profile. */
            int p = s_trying;
            if (p == PROFILE_CANDIDATE) {
                int slot = store_candidate();
                lock();
                s_cand_ssid[0] = '\0'; s_cand_pass[0] = '\0';
                s_join = (slot >= 0) ? EYE_NET_JOIN_OK : EYE_NET_JOIN_FAILED;
                s_join_slot = slot;
                unlock();
                /* If the write failed the link is still good, so keep it; it
                 * just stays a candidate, and a later disconnect falls back. */
                if (slot >= 0) p = slot;
            }

            lock();
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&cmd.ip));
            s_state = EYE_NET_STA_CONNECTED;
            s_retries = 0; s_tried = 0; s_sweep_idle = false;
            s_trying = p;
            if (p >= 0) s_active = p;
            unlock();

            if (p >= 0) {
                ESP_LOGI(TAG, "profile %d up — control page at http://" IPSTR "/",
                         p + 1, IP2STR(&cmd.ip));
                /* Only a profile that actually worked becomes the boot default. */
                persist_active(p);
            } else {
                /* An IP with no attempt behind it: the driver reconnected on
                 * its own. Take the SSID from the radio rather than leaving
                 * s_ssid claiming the access point. */
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    lock();
                    strlcpy(s_ssid, (const char *)ap.ssid, sizeof(s_ssid));
                    unlock();
                }
                ESP_LOGW(TAG, "connected but not stored — control page at http://" IPSTR "/",
                         IP2STR(&cmd.ip));
            }
            break;
        }

        case CMD_CONNECT:
            s_tried = 0;
            if (start_profile(cmd.profile) != ESP_OK) enter_ap_only(true);
            break;

        case CMD_JOIN:
            s_tried = 0;
            if (start_candidate() != ESP_OK) enter_ap_only(true);
            break;

        case CMD_START_AP:      enter_ap_only(false); break;
        case CMD_SCAN:          run_scan();           break;
        case CMD_DISCONNECTED:  handle_disconnect();  break;
        }
    }
}

/* ------------------------------------------------------------------- api */

/* esp_netif's default softAP address has to be overridden explicitly; setting
 * AP_IP_ADDR alone would only change what the firmware claims. */
static esp_err_t configure_ap_address(void)
{
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(AP_IP_ADDR);
    ip.gw.addr      = esp_ip4addr_aton(AP_IP_ADDR);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);

    /* The DHCP server advertises its own address as the gateway, so it cannot
     * be running while that address changes. Already stopped is fine — nothing
     * has started it yet on the first call. */
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(err, TAG, "dhcps stop");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip), TAG, "ap ip");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "dhcps start");
    return ESP_OK;
}

static esp_err_t configure_softap(void)
{
    const size_t plen = strlen(CONFIG_EYE_NET_AP_PASSWORD);
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CONFIG_EYE_NET_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_EYE_NET_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len       = strlen(CONFIG_EYE_NET_AP_SSID);
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode       = (plen >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (plen > 0 && plen < 8) {
        ESP_LOGW(TAG, "AP password under 8 chars — access point will be OPEN, "
                      "and anyone on it can drive the servos");
    }
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

esp_err_t eye_net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    s_lock = xSemaphoreCreateMutex();
    s_q    = xQueueCreate(8, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_lock && s_q, ESP_ERR_NO_MEM, TAG, "alloc");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && s_ap_netif, ESP_ERR_NO_MEM, TAG, "netifs");
    esp_netif_set_hostname(s_sta_netif, CONFIG_EYE_NET_HOSTNAME);
    ESP_RETURN_ON_ERROR(configure_ap_address(), TAG, "ap address");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL), TAG, "ip events");

    load_credentials();

    /* APSTA for the whole run. The recovery AP never goes away. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(configure_softap(), TAG, "ap config");

    if (s_active >= 0) {
        wifi_config_t cfg = { 0 };
        s_trying = s_active;
        s_state  = EYE_NET_CONNECTING;
        strlcpy(s_ssid, s_prof_ssid[s_active], sizeof(s_ssid));
        strlcpy((char *)cfg.sta.ssid, s_prof_ssid[s_active], sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, s_prof_pass[s_active], sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = s_prof_pass[s_active][0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "sta config");
    } else {
        s_trying = -1;
        s_state  = EYE_NET_AP_ONLY;
        strlcpy(s_ssid, CONFIG_EYE_NET_AP_SSID, sizeof(s_ssid));
        strlcpy(s_ip, AP_IP_ADDR, sizeof(s_ip));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    mdns_start();
    ESP_LOGI(TAG, "APSTA up: AP '%s' at %s, %d station profile(s)",
             CONFIG_EYE_NET_AP_SSID, AP_IP_ADDR, configured_count());

    ESP_RETURN_ON_FALSE(
        xTaskCreate(manager_task, "eye_net", 4096, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "manager task");
    return ESP_OK;
}

eye_net_state_t eye_net_state(void) { return s_state; }
const char *eye_net_ap_ssid(void)   { return CONFIG_EYE_NET_AP_SSID; }
int eye_net_active_profile(void)    { return s_active; }

int eye_net_ssid(char *buf, size_t len)
{
    lock(); int n = snprintf(buf, len, "%s", s_ssid); unlock();
    return (n < 0) ? 0 : n;
}

int eye_net_ip(char *buf, size_t len)
{
    lock(); int n = snprintf(buf, len, "%s", s_ip); unlock();
    return (n < 0) ? 0 : n;
}

int eye_net_ap_clients(void)
{
    wifi_sta_list_t list;
    return (esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? list.num : 0;
}

int eye_net_profile_ssid(int n, char *buf, size_t len)
{
    if (n < 0 || n >= EYE_NET_PROFILES) return 0;
    lock(); int r = snprintf(buf, len, "%s", s_prof_ssid[n]); unlock();
    return (r < 0) ? 0 : r;
}

/* Writes a slot outright, without trying the credentials first. eye_net_join()
 * is the path that verifies; this one is for deliberate slot management. */
esp_err_t eye_net_set_profile(int n, const char *ssid, const char *password)
{
    if (n < 0 || n >= EYE_NET_PROFILES || ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= sizeof(s_prof_ssid[0])) return ESP_ERR_INVALID_SIZE;
    if (password && strlen(password) >= sizeof(s_prof_pass[0])) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = write_profile(n, ssid, password);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "profile %d saved (SSID '%s')", n + 1, ssid);   /* never the password */
    return ESP_OK;
}

esp_err_t eye_net_clear_profile(int n)
{
    if (n < 0 || n >= EYE_NET_PROFILES) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    nvs_erase_key(h, s_ssid_keys[n]);
    nvs_erase_key(h, s_pass_keys[n]);
    nvs_erase_key(h, s_seq_keys[n]);
    nvs_commit(h);
    nvs_close(h);

    lock();
    s_prof_ssid[n][0] = '\0';
    s_prof_pass[n][0] = '\0';
    s_prof_seq[n] = 0;
    if (s_active == n) s_active = -1;
    unlock();
    /* A currently-connected profile keeps its link; it just stops being a
     * sweep candidate. */
    return ESP_OK;
}

esp_err_t eye_net_connect_profile(int n)
{
    if (n < 0 || n >= EYE_NET_PROFILES || !s_prof_ssid[n][0]) return ESP_ERR_INVALID_ARG;
    const cmd_t c = { .type = CMD_CONNECT, .profile = n };
    return (xQueueSend(s_q, &c, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int eye_net_oldest_profile(void)
{
    int oldest = -1;
    lock();
    for (int i = 0; i < EYE_NET_PROFILES; i++) {
        if (!s_prof_ssid[i][0]) continue;
        if (oldest < 0 || s_prof_seq[i] < s_prof_seq[oldest]) oldest = i;
    }
    unlock();
    return oldest;
}

esp_err_t eye_net_join(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(s_cand_ssid)) return ESP_ERR_INVALID_SIZE;
    if (password && strlen(password) >= sizeof(s_cand_pass)) return ESP_ERR_INVALID_SIZE;

    lock();
    if (s_join == EYE_NET_JOIN_BUSY) { unlock(); return ESP_ERR_INVALID_STATE; }
    strlcpy(s_cand_ssid, ssid, sizeof(s_cand_ssid));
    strlcpy(s_cand_pass, password ? password : "", sizeof(s_cand_pass));
    /* Set busy before queueing, so a caller polling the result cannot read
     * the previous attempt's verdict and believe it. */
    s_join = EYE_NET_JOIN_BUSY;
    s_join_slot = -1;
    unlock();

    const cmd_t c = { .type = CMD_JOIN };
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {
        lock(); s_join = EYE_NET_JOIN_IDLE; unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

eye_net_join_state_t eye_net_join_result(int *slot)
{
    lock();
    eye_net_join_state_t st = s_join;
    if (slot) *slot = s_join_slot;
    unlock();
    return st;
}

esp_err_t eye_net_force_ap(void)
{
    const cmd_t c = { .type = CMD_START_AP };
    return (xQueueSend(s_q, &c, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t eye_net_scan_start(void)
{
    lock();
    if (s_scan_busy) { unlock(); return ESP_ERR_INVALID_STATE; }
    s_scan_busy = true;
    unlock();
    const cmd_t c = { .type = CMD_SCAN };
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {
        lock(); s_scan_busy = false; unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool eye_net_scan_busy(void)
{
    lock(); bool b = s_scan_busy; unlock();
    return b;
}

int eye_net_scan_results(eye_net_scan_entry_t *out, int max)
{
    if (out == NULL || max <= 0) return 0;
    lock();
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan, (size_t)n * sizeof(*out));
    unlock();
    return n;
}
