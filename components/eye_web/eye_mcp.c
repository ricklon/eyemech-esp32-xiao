#include "eye_mcp.h"
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_vision.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"

#ifndef EYEMECH_VERSION
#define EYEMECH_VERSION "dev"
#endif

#define MCP_MODERN        "2026-07-28"
#define META_VERSION      "io.modelcontextprotocol/protocolVersion"
#define META_SERVER_INFO  "io.modelcontextprotocol/serverInfo"

/* JSON-RPC and MCP error codes. -32020..-32099 are reserved by MCP 2026-07-28. */
#define ERR_PARSE           -32700
#define ERR_INVALID_REQUEST -32600
#define ERR_METHOD          -32601
#define ERR_PARAMS          -32602
#define ERR_HEADER_MISMATCH -32020
#define ERR_VERSION         -32022

/* Newest first. The first entry is what a legacy client gets when it asks for a
 * version this server does not know. */
static const char *const s_legacy_versions[] = { "2025-11-25", "2025-06-18", "2025-03-26" };
#define LEGACY_COUNT (sizeof(s_legacy_versions) / sizeof(s_legacy_versions[0]))

/* Read by the model. The facts here are the ones a caller gets wrong without
 * them: there is no feedback, gaze units are not degrees, and some things only a
 * person may do. */
static const char *const s_instructions =
    "Animatronic eyes: two gaze axes and four eyelids on hobby servos with no "
    "position feedback, so every position reported is the last one commanded, not a "
    "measurement. Gaze is 0..1 across each axis's calibrated range; which physical "
    "direction 0 means depends on how the servo is mounted, so check with a person "
    "before relying on left/right or up/down. Motion tools are refused while the "
    "servos are released or the mechanism is in standby or calibration mode, and only a person "
    "at the control page or serial console can change either. release is the "
    "software stop: it latches, and this server cannot undo it.";

/* ------------------------------------------------------------ JSON-RPC glue */

static cJSON *rpc_error(const cJSON *id, int code, const char *message, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON *err = cJSON_AddObjectToObject(root, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    if (data) cJSON_AddItemToObject(err, "data", data);
    return root;
}

static void finish(eye_mcp_reply_t *out, int status, cJSON *root)
{
    out->status = status;
    out->body = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
}

/* Legacy results carry it as `serverInfo`; modern ones under `_meta` with the
 * namespaced key. */
static void add_server_info(cJSON *obj, const char *key)
{
    cJSON *info = cJSON_AddObjectToObject(obj, key);
    cJSON_AddStringToObject(info, "name", "eyemech");
    cJSON_AddStringToObject(info, "version", EYEMECH_VERSION);
}

/* ------------------------------------------------------------------ helpers */

/* Mcp-Name may arrive as =?base64?...?= when the value is not header-safe.
 * Returns a malloc'd decoded copy, or NULL if the encoding is malformed. */
static char *decode_header_value(const char *v)
{
    static const char prefix[] = "=?base64?";
    size_t len = strlen(v), plen = sizeof(prefix) - 1;
    if (len < plen + 2 || strncmp(v, prefix, plen) != 0 || strcmp(v + len - 2, "?=") != 0) {
        return strdup(v);
    }
    const char *src = v + plen;
    size_t n = len - plen - 2;
    char *dst = malloc(n + 1);
    if (!dst) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        int val;
        if (c >= 'A' && c <= 'Z')      val = c - 'A';
        else if (c >= 'a' && c <= 'z') val = c - 'a' + 26;
        else if (c >= '0' && c <= '9') val = c - '0' + 52;
        else if (c == '+')             val = 62;
        else if (c == '/')             val = 63;
        else if (c == '=')             break;
        else { free(dst); return NULL; }
        acc = (acc << 6) | (unsigned)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    dst[o] = '\0';
    return dst;
}

/* DNS rebinding guard: a browser page on another origin must not be able to
 * drive the servos. Non-browser clients send no Origin and are unaffected. */
static bool origin_ok(const eye_mcp_headers_t *h)
{
    if (!h->origin) return true;
    if (!h->host) return false;
    const char *o = h->origin;
    if (strncmp(o, "http://", 7) == 0)       o += 7;
    else if (strncmp(o, "https://", 8) == 0) o += 8;
    else return false;
    return strcasecmp(o, h->host) == 0;
}

static float to_unit(eye_servo_id_t id, float deg)
{
    eye_limits_t l = eye_servo_limits(id);
    float span = l.max - l.min;   /* may be negative: limits can run backwards */
    if (!isfinite(deg) || fabsf(span) < 0.001f) return NAN;
    return (deg - l.min) / span;
}

static float from_unit(eye_servo_id_t id, float t)
{
    eye_limits_t l = eye_servo_limits(id);
    return l.min + t * (l.max - l.min);
}

/* NULL when motion is allowed, otherwise the reason it is not. */
static const char *motion_refusal(void)
{
    if (eye_servo_is_released()) {
        return "The servos are released (a latched stop). Nothing moves until a person "
               "engages them from the control page or serial console; this server "
               "cannot engage.";
    }
    if (eye_motion_get_mode() == EYE_MODE_FOLLOW) {
        return "The eyes are following a live pose stream, and a motion command would fight "
               "it. It returns to its previous mode when the stream stops.";
    }
    if (eye_motion_get_mode() == EYE_MODE_STANDBY) {
        return "The mechanism is in standby: stopped, with nothing scheduled. A person has "
               "to pick a mode at the control page or serial console before it moves.";
    }
    if (eye_motion_get_mode() == EYE_MODE_CALIBRATION) {
        return "The mechanism is in calibration mode, which is for a person fitting horns "
               "and measuring limits. A person has to leave it from the control page or "
               "serial console.";
    }
    return NULL;
}

/* ------------------------------------------------------------- tool results */

static cJSON *tool_text(const char *text, bool is_error)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(r, "isError", is_error);
    return r;
}

/* -------------------------------------------------------------------- tools */

static cJSON *state_object(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "mode", eye_motion_mode_name(eye_motion_get_mode()));
    cJSON_AddBoolToObject(s, "released", eye_servo_is_released());
    cJSON_AddBoolToObject(s, "safe_boot", eye_servo_safe_boot());
    const char *playing = eye_motion_anim_playing();
    cJSON_AddItemToObject(s, "animation", playing ? cJSON_CreateString(playing) : cJSON_CreateNull());
    cJSON_AddBoolToObject(s, "vision", eye_vision_present());

    cJSON *gaze = cJSON_AddObjectToObject(s, "gaze");
    cJSON_AddNumberToObject(gaze, "lr", to_unit(EYE_LR, eye_motion_target_lr()));
    cJSON_AddNumberToObject(gaze, "ud", to_unit(EYE_UD, eye_motion_target_ud()));
    cJSON_AddNumberToObject(s, "lid_trim", eye_motion_get_lid_trim());

    cJSON *servos = cJSON_AddArrayToObject(s, "servos");
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        eye_limits_t l = eye_servo_limits((eye_servo_id_t)i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", eye_servo_name((eye_servo_id_t)i));
        cJSON_AddNumberToObject(o, "commanded_deg", eye_servo_read((eye_servo_id_t)i));  /* NAN -> null */
        cJSON_AddNumberToObject(o, "min", l.min);
        cJSON_AddNumberToObject(o, "max", l.max);
        cJSON_AddItemToArray(servos, o);
    }

    cJSON *anims = cJSON_AddArrayToObject(s, "animations");
    for (const char *const *a = eye_motion_anim_names(); *a; a++) {
        cJSON_AddItemToArray(anims, cJSON_CreateString(*a));
    }
    return s;
}

static cJSON *call_get_state(const cJSON *args)
{
    (void)args;
    cJSON *state = state_object();
    char *text = cJSON_PrintUnformatted(state);
    cJSON *r = tool_text(text ? text : "{}", false);
    cJSON_free(text);
    cJSON_AddItemToObject(r, "structuredContent", state);
    return r;
}

static bool unit_arg(const cJSON *args, const char *key, float *out, bool *present, char *err, size_t errlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    *present = v != NULL && !cJSON_IsNull(v);
    if (!*present) return true;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0.0 || v->valuedouble > 1.0) {
        snprintf(err, errlen, "%s must be a number from 0 to 1", key);
        return false;
    }
    *out = (float)v->valuedouble;
    return true;
}

static cJSON *call_look(const cJSON *args)
{
    const char *no = motion_refusal();
    if (no) return tool_text(no, true);

    char err[64];
    float lr = 0, ud = 0;
    bool has_lr, has_ud;
    if (!unit_arg(args, "lr", &lr, &has_lr, err, sizeof(err)) ||
        !unit_arg(args, "ud", &ud, &has_ud, err, sizeof(err))) {
        return tool_text(err, true);
    }
    if (!has_lr && !has_ud) return tool_text("give lr, ud, or both", true);

    float lr_deg = has_lr ? from_unit(EYE_LR, lr) : eye_motion_target_lr();
    float ud_deg = has_ud ? from_unit(EYE_UD, ud) : eye_motion_target_ud();
    eye_motion_set_mode(EYE_MODE_MANUAL);
    if (eye_motion_look(lr_deg, ud_deg) != ESP_OK) return tool_text("the motion layer refused the look", true);

    char msg[160];
    snprintf(msg, sizeof(msg), "Commanded gaze lr=%.2f (%.1f deg), ud=%.2f (%.1f deg). Mode is now manual.",
             to_unit(EYE_LR, eye_motion_target_lr()), eye_motion_target_lr(),
             to_unit(EYE_UD, eye_motion_target_ud()), eye_motion_target_ud());
    return tool_text(msg, false);
}

static cJSON *call_blink(const cJSON *args)
{
    (void)args;
    const char *no = motion_refusal();
    if (no) return tool_text(no, true);
    eye_motion_request_blink();
    return tool_text("Blink queued.", false);
}

static cJSON *call_play_animation(const cJSON *args)
{
    const char *no = motion_refusal();
    if (no) return tool_text(no, true);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
    const cJSON *rep  = cJSON_GetObjectItemCaseSensitive(args, "repeat");
    if (!cJSON_IsString(name)) return tool_text("name is required", true);
    int repeat = 1;
    if (rep && !cJSON_IsNull(rep)) {
        if (!cJSON_IsNumber(rep) || rep->valuedouble != (int)rep->valuedouble ||
            rep->valuedouble < 1 || rep->valuedouble > 10) {
            return tool_text("repeat must be a whole number from 1 to 10", true);
        }
        repeat = (int)rep->valuedouble;
    }
    if (eye_motion_play(name->valuestring, repeat) != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "no animation called '%.40s'", name->valuestring);
        return tool_text(msg, true);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Playing %.40s x%d; the previous mode resumes when it ends.",
             name->valuestring, repeat);
    return tool_text(msg, false);
}

static cJSON *call_stop_animation(const cJSON *args)
{
    (void)args;
    (void)eye_motion_anim_stop();   /* idempotent: nothing playing is not an error */
    return tool_text("Animation stop requested; it settles after the frame in flight.", false);
}

static cJSON *call_set_mode(const cJSON *args)
{
    const char *no = motion_refusal();
    if (no) return tool_text(no, true);

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "mode");
    const char *name = cJSON_IsString(m) ? m->valuestring : "";
    eye_mode_t mode;
    if (strcmp(name, "auto") == 0)          mode = EYE_MODE_AUTO;
    else if (strcmp(name, "manual") == 0)   mode = EYE_MODE_MANUAL;
    else if (strcmp(name, "tracking") == 0) mode = EYE_MODE_TRACKING;
    else return tool_text("mode must be auto, manual or tracking", true);

    eye_motion_set_mode(mode);
    if (mode == EYE_MODE_TRACKING && !eye_vision_present()) {
        return tool_text("Mode is now tracking, but no vision module is detected, so the "
                         "eyes will not follow anything.", false);
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "Mode is now %s.", name);
    return tool_text(msg, false);
}

static cJSON *call_release(const cJSON *args)
{
    (void)args;
    if (eye_servo_release_all() != ESP_OK) return tool_text("release failed on the I2C bus", true);
    return tool_text("All servos released and latched. A person must engage them from the "
                     "control page or serial console before anything moves again.", false);
}

/* ------------------------------------------------------------- tool table */

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    bool read_only, destructive, idempotent;
    cJSON *(*schema)(void);
    cJSON *(*call)(const cJSON *args);
} tool_t;

static cJSON *obj_schema(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddObjectToObject(s, "properties");
    return s;
}

static cJSON *empty_schema(void)
{
    cJSON *s = obj_schema();
    cJSON_AddBoolToObject(s, "additionalProperties", false);
    return s;
}

static void add_prop(cJSON *schema, const char *key, cJSON *prop)
{
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(schema, "properties"), key, prop);
}

static cJSON *unit_prop(const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "number");
    cJSON_AddNumberToObject(p, "minimum", 0);
    cJSON_AddNumberToObject(p, "maximum", 1);
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

static cJSON *look_schema(void)
{
    cJSON *s = empty_schema();
    add_prop(s, "lr", unit_prop("Horizontal gaze across the calibrated range, 0.5 centred. Omit to keep."));
    add_prop(s, "ud", unit_prop("Vertical gaze across the calibrated range, 0.5 level; the lids follow. Omit to keep."));
    return s;
}

static cJSON *anim_schema(void)
{
    cJSON *s = empty_schema();
    cJSON *name = cJSON_CreateObject();
    cJSON_AddStringToObject(name, "type", "string");
    cJSON *names = cJSON_AddArrayToObject(name, "enum");
    /* One "name: what it looks like" line per animation, from the same table the
     * control page labels its buttons with, so a model can choose by effect. */
    size_t len = 1;
    for (const char *const *a = eye_motion_anim_names(); *a; a++) {
        cJSON_AddItemToArray(names, cJSON_CreateString(*a));
        len += strlen(*a) + strlen(eye_motion_anim_desc(*a)) + 3;
    }
    char *desc = malloc(len);
    if (desc) {
        size_t o = 0;
        for (const char *const *a = eye_motion_anim_names(); *a; a++) {
            o += (size_t)snprintf(desc + o, len - o, "%s%s: %s", o ? "\n" : "", *a,
                                  eye_motion_anim_desc(*a));
        }
        cJSON_AddStringToObject(name, "description", desc);
        free(desc);
    }
    add_prop(s, "name", name);
    cJSON *rep = cJSON_CreateObject();
    cJSON_AddStringToObject(rep, "type", "integer");
    cJSON_AddNumberToObject(rep, "minimum", 1);
    cJSON_AddNumberToObject(rep, "maximum", 10);
    cJSON_AddNumberToObject(rep, "default", 1);
    add_prop(s, "repeat", rep);
    cJSON *req = cJSON_AddArrayToObject(s, "required");
    cJSON_AddItemToArray(req, cJSON_CreateString("name"));
    return s;
}

static cJSON *mode_schema(void)
{
    cJSON *s = empty_schema();
    cJSON *mode = cJSON_CreateObject();
    cJSON_AddStringToObject(mode, "type", "string");
    cJSON *e = cJSON_AddArrayToObject(mode, "enum");
    cJSON_AddItemToArray(e, cJSON_CreateString("auto"));
    cJSON_AddItemToArray(e, cJSON_CreateString("manual"));
    cJSON_AddItemToArray(e, cJSON_CreateString("tracking"));
    cJSON_AddStringToObject(mode, "description",
        "auto: random gaze and blinks. manual: holds where look puts it. "
        "tracking: follows the vision module, if one is fitted.");
    add_prop(s, "mode", mode);
    cJSON *req = cJSON_AddArrayToObject(s, "required");
    cJSON_AddItemToArray(req, cJSON_CreateString("mode"));
    return s;
}

static const tool_t s_tools[] = {
    { "get_state", "Eye state",
      "Mode, release latch, commanded gaze (0..1) and per-servo commanded angles and "
      "calibrated limits. Positions are commanded, not measured: there is no feedback.",
      true, false, true, empty_schema, call_get_state },
    { "look", "Look",
      "Point the eyes. Switches to manual mode. Refused while released, in standby or in calibration mode.",
      false, false, true, look_schema, call_look },
    { "blink", "Blink", "Blink once. Refused while released, in standby or in calibration mode.",
      false, false, false, empty_schema, call_blink },
    { "play_animation", "Play animation",
      "Play a built-in expression; the previous mode resumes when it ends. Refused while "
      "released, in standby or in calibration mode.",
      false, false, false, anim_schema, call_play_animation },
    { "stop_animation", "Stop animation",
      "End the playing animation after the frame in flight. Safe to call when nothing is playing.",
      false, false, true, empty_schema, call_stop_animation },
    { "set_mode", "Set mode",
      "Switch between auto, manual and tracking. Standby and calibration are not available "
      "here, and it is refused while released, in standby or in calibration mode.",
      false, false, true, mode_schema, call_set_mode },
    { "release", "Release servos",
      "Software stop: every servo goes limp and stays limp until a person engages them "
      "at the control page or serial console. This server cannot undo it.",
      false, true, true, empty_schema, call_release },
};
#define TOOL_COUNT (sizeof(s_tools) / sizeof(s_tools[0]))

static cJSON *tools_list(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "tools");
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        const tool_t *t = &s_tools[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "title", t->title);
        cJSON_AddStringToObject(o, "description", t->description);
        cJSON_AddItemToObject(o, "inputSchema", t->schema());
        cJSON *ann = cJSON_AddObjectToObject(o, "annotations");
        cJSON_AddBoolToObject(ann, "readOnlyHint", t->read_only);
        cJSON_AddBoolToObject(ann, "destructiveHint", t->destructive);
        cJSON_AddBoolToObject(ann, "idempotentHint", t->idempotent);
        cJSON_AddBoolToObject(ann, "openWorldHint", false);
        cJSON_AddItemToArray(arr, o);
    }
    return r;
}

/* ------------------------------------------------------------- dispatch */

static bool header_is(const char *hdr, const char *expected)
{
    if (!hdr || !expected) return false;
    char *decoded = decode_header_value(hdr);
    bool same = decoded && strcmp(decoded, expected) == 0;
    free(decoded);
    return same;
}

void eye_mcp_handle(const char *body, size_t len, const eye_mcp_headers_t *hdr,
                    eye_mcp_reply_t *out)
{
    out->status = 200;
    out->body = NULL;

    if (!origin_ok(hdr)) {
        finish(out, 403, rpc_error(NULL, ERR_INVALID_REQUEST, "Origin not allowed", NULL));
        return;
    }

    cJSON *req = cJSON_ParseWithLength(body, len);
    if (!req) {
        finish(out, 400, rpc_error(NULL, ERR_PARSE, "Parse error", NULL));
        return;
    }
    const cJSON *method_j = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (!cJSON_IsObject(req) || !cJSON_IsString(method_j)) {
        /* Batches were dropped in 2025-06-18 and are not accepted here. */
        finish(out, 400, rpc_error(NULL, ERR_INVALID_REQUEST, "Invalid request", NULL));
        cJSON_Delete(req);
        return;
    }
    const char *method = method_j->valuestring;

    if (id == NULL) {   /* a notification, e.g. legacy notifications/initialized */
        out->status = 202;
        cJSON_Delete(req);
        return;
    }

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(meta, META_VERSION);
    bool modern = version != NULL;

    if (modern) {
        if (!cJSON_IsString(version) || strcmp(version->valuestring, MCP_MODERN) != 0) {
            cJSON *data = cJSON_CreateObject();
            cJSON *sup = cJSON_AddArrayToObject(data, "supported");
            cJSON_AddItemToArray(sup, cJSON_CreateString(MCP_MODERN));
            for (size_t i = 0; i < LEGACY_COUNT; i++) {
                cJSON_AddItemToArray(sup, cJSON_CreateString(s_legacy_versions[i]));
            }
            cJSON_AddItemToObject(data, "requested",
                cJSON_IsString(version) ? cJSON_CreateString(version->valuestring) : cJSON_CreateNull());
            finish(out, 400, rpc_error(id, ERR_VERSION, "Unsupported protocol version", data));
            cJSON_Delete(req);
            return;
        }
        /* The headers exist so intermediaries can route without parsing the body;
         * a mismatch means something between client and here disagrees about what
         * this request is, so refuse it rather than pick one. */
        const char *mismatch = NULL;
        if (!header_is(hdr->protocol_version, MCP_MODERN)) mismatch = "MCP-Protocol-Version";
        else if (!header_is(hdr->method, method))          mismatch = "Mcp-Method";
        else if (strcmp(method, "tools/call") == 0) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(params, "name");
            if (!cJSON_IsString(n) || !header_is(hdr->name, n->valuestring)) mismatch = "Mcp-Name";
        }
        if (mismatch) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Header mismatch: %s is missing or does not match the body", mismatch);
            finish(out, 400, rpc_error(id, ERR_HEADER_MISMATCH, msg, NULL));
            cJSON_Delete(req);
            return;
        }
    }

    cJSON *result = NULL;
    if (!modern && strcmp(method, "initialize") == 0) {
        const cJSON *asked = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
        const char *chosen = s_legacy_versions[0];
        for (size_t i = 0; cJSON_IsString(asked) && i < LEGACY_COUNT; i++) {
            if (strcmp(asked->valuestring, s_legacy_versions[i]) == 0) chosen = s_legacy_versions[i];
        }
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", chosen);
        cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
        cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "tools"), "listChanged", false);
        add_server_info(result, "serverInfo");
        cJSON_AddStringToObject(result, "instructions", s_instructions);
    } else if (!modern && strcmp(method, "ping") == 0) {
        result = cJSON_CreateObject();   /* removed in 2026-07-28, still legacy */
    } else if (strcmp(method, "server/discover") == 0) {
        result = cJSON_CreateObject();
        cJSON *sup = cJSON_AddArrayToObject(result, "supportedVersions");
        cJSON_AddItemToArray(sup, cJSON_CreateString(MCP_MODERN));
        for (size_t i = 0; i < LEGACY_COUNT; i++) {
            cJSON_AddItemToArray(sup, cJSON_CreateString(s_legacy_versions[i]));
        }
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(result, "capabilities"), "tools");
        cJSON_AddStringToObject(result, "instructions", s_instructions);
        cJSON_AddNumberToObject(result, "ttlMs", 3600000);
        cJSON_AddStringToObject(result, "cacheScope", "private");
    } else if (strcmp(method, "tools/list") == 0) {
        result = tools_list();
        if (modern) {
            /* The table is compiled in, so the list only changes with a reflash. */
            cJSON_AddNumberToObject(result, "ttlMs", 3600000);
            cJSON_AddStringToObject(result, "cacheScope", "private");
        }
    } else if (strcmp(method, "tools/call") == 0) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const tool_t *tool = NULL;
        for (size_t i = 0; cJSON_IsString(name) && i < TOOL_COUNT; i++) {
            if (strcmp(name->valuestring, s_tools[i].name) == 0) tool = &s_tools[i];
        }
        if (!tool) {
            finish(out, 200, rpc_error(id, ERR_PARAMS, "Unknown tool", NULL));
            cJSON_Delete(req);
            return;
        }
        if (args != NULL && !cJSON_IsObject(args)) {
            finish(out, 200, rpc_error(id, ERR_PARAMS, "arguments must be an object", NULL));
            cJSON_Delete(req);
            return;
        }
        result = tool->call(args);
    } else {
        /* Modern servers answer an unknown method with 404. A legacy client may read
         * 404 as "session expired" and re-initialize forever, so it gets 200. */
        finish(out, modern ? 404 : 200, rpc_error(id, ERR_METHOD, "Method not found", NULL));
        cJSON_Delete(req);
        return;
    }

    if (modern) {
        cJSON_AddStringToObject(result, "resultType", "complete");
        add_server_info(cJSON_AddObjectToObject(result, "_meta"), META_SERVER_INFO);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    cJSON_AddItemToObject(root, "result", result);
    finish(out, 200, root);
    cJSON_Delete(req);
}
