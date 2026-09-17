/* Protocol tests for components/eye_web/eye_mcp.c. Run with run.sh. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "eye_mcp.h"
#include "fakes.h"

static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    int    status;
    cJSON *json;   /* NULL when no body */
} resp_t;

static resp_t post(const char *body, const eye_mcp_headers_t *hdr)
{
    static const eye_mcp_headers_t none = { 0 };
    eye_mcp_reply_t r;
    eye_mcp_handle(body, strlen(body), hdr ? hdr : &none, &r);
    resp_t out = { r.status, r.body ? cJSON_Parse(r.body) : NULL };
    if (r.body && !out.json) {
        fprintf(stderr, "  FAIL: server produced invalid JSON: %s\n", r.body);
        s_failures++;
    }
    free(r.body);
    return out;
}

/* A modern request: _meta in the body, headers to match. `tool` and `args` are
 * for tools/call and may be NULL otherwise. */
static resp_t modern(const char *method, const char *tool, const char *args)
{
    char name_part[128] = "";
    if (tool) {
        snprintf(name_part, sizeof(name_part), "\"name\":\"%s\",\"arguments\":%s,",
                 tool, args ? args : "{}");
    }
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{%s"
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}",
             method, name_part);
    eye_mcp_headers_t h = { .protocol_version = "2026-07-28", .method = method, .name = tool };
    return post(body, &h);
}

static const cJSON *path(const cJSON *j, const char *a, const char *b)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, a);
    return b ? cJSON_GetObjectItemCaseSensitive(x, b) : x;
}

static const char *tool_text(const resp_t *r)
{
    const cJSON *content = path(r->json, "result", "content");
    const cJSON *item = cJSON_GetArrayItem(content, 0);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "text");
    return cJSON_IsString(t) ? t->valuestring : "";
}

static int tool_is_error(const resp_t *r)
{
    return cJSON_IsTrue(path(r->json, "result", "isError"));
}

static int error_code(const resp_t *r)
{
    const cJSON *c = path(r->json, "error", "code");
    return cJSON_IsNumber(c) ? c->valueint : 0;
}

static void done(resp_t *r) { cJSON_Delete(r->json); }

/* ------------------------------------------------------------------ tests */

static void test_legacy_initialize_and_list(void)
{
    fake_reset();
    resp_t r = post("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
                    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                    "\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}}", NULL);
    CHECK(r.status == 200);
    CHECK(strcmp(path(r.json, "result", "protocolVersion")->valuestring, "2025-06-18") == 0);
    CHECK(cJSON_IsObject(path(path(r.json, "result", "capabilities"), "tools", NULL)));
    CHECK(strcmp(path(path(r.json, "result", "serverInfo"), "name", NULL)->valuestring, "eyemech") == 0);
    CHECK(path(r.json, "result", "resultType") == NULL);   /* legacy results stay legacy-shaped */
    done(&r);

    r = post("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}", NULL);
    CHECK(strcmp(path(r.json, "result", "protocolVersion")->valuestring, "2025-11-25") == 0);
    done(&r);

    r = post("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL);
    CHECK(r.status == 202 && r.json == NULL);

    r = post("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}", NULL);
    const cJSON *tools = path(r.json, "result", "tools");
    CHECK(cJSON_GetArraySize(tools) == 7);
    CHECK(strcmp(cJSON_GetArrayItem(tools, 0)->child->valuestring, "get_state") == 0);
    int saw_engage = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const char *n = cJSON_GetObjectItemCaseSensitive(t, "name")->valuestring;
        if (strstr(n, "engage") || strstr(n, "calib") || strstr(n, "servo") || strstr(n, "limit")) saw_engage = 1;
        CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(t, "inputSchema")));
    }
    CHECK(!saw_engage);
    CHECK(path(r.json, "result", "ttlMs") == NULL);
    done(&r);

    r = post("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}", NULL);
    CHECK(r.status == 200 && cJSON_IsObject(path(r.json, "result", NULL)));
    done(&r);
}

static void test_modern_discover_and_list(void)
{
    fake_reset();
    resp_t r = modern("server/discover", NULL, NULL);
    CHECK(r.status == 200);
    const cJSON *res = path(r.json, "result", NULL);
    CHECK(strcmp(cJSON_GetArrayItem(path(res, "supportedVersions", NULL), 0)->valuestring, "2026-07-28") == 0);
    CHECK(strcmp(path(res, "resultType", NULL)->valuestring, "complete") == 0);
    CHECK(strcmp(path(path(res, "_meta", "io.modelcontextprotocol/serverInfo"), "name", NULL)->valuestring, "eyemech") == 0);
    CHECK(cJSON_IsString(path(res, "instructions", NULL)));
    done(&r);

    r = modern("tools/list", NULL, NULL);
    CHECK(r.status == 200);
    CHECK(cJSON_IsNumber(path(r.json, "result", "ttlMs")));
    CHECK(strcmp(path(r.json, "result", "cacheScope")->valuestring, "private") == 0);
    done(&r);

    r = modern("ping", NULL, NULL);   /* removed from the modern protocol */
    CHECK(r.status == 404 && error_code(&r) == -32601);
    done(&r);
}

static void test_modern_header_validation(void)
{
    fake_reset();
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"blink\","
                       "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}";
    eye_mcp_headers_t h = { .protocol_version = "2026-07-28", .method = "tools/call", .name = "release" };
    resp_t r = post(body, &h);
    CHECK(r.status == 400 && error_code(&r) == -32020);
    CHECK(fake.blinks == 0 && fake.release_calls == 0);   /* neither tool ran */
    done(&r);

    h.name = NULL;
    r = post(body, &h);
    CHECK(r.status == 400 && error_code(&r) == -32020);
    done(&r);

    h.name = "=?base64?Ymxpbms=?=";   /* "blink", encoded */
    r = post(body, &h);
    CHECK(r.status == 200 && !tool_is_error(&r) && fake.blinks == 1);
    done(&r);

    eye_mcp_headers_t no_version = { .method = "tools/call", .name = "blink" };
    r = post(body, &no_version);
    CHECK(r.status == 400 && error_code(&r) == -32020);
    done(&r);

    r = post("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{"
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2030-01-01\"}}}", &h);
    CHECK(r.status == 400 && error_code(&r) == -32022);
    CHECK(cJSON_GetArraySize(path(path(r.json, "error", "data"), "supported", NULL)) == 4);
    done(&r);
}

static void test_origin_and_malformed(void)
{
    fake_reset();
    eye_mcp_headers_t evil = { .origin = "http://evil.example", .host = "eyemech.local" };
    resp_t r = post("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"release\"}}", &evil);
    CHECK(r.status == 403 && fake.release_calls == 0);
    done(&r);

    eye_mcp_headers_t same = { .origin = "http://EyeMech.local", .host = "eyemech.local" };
    r = post("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", &same);
    CHECK(r.status == 200);
    done(&r);

    r = post("{not json", NULL);
    CHECK(r.status == 400 && error_code(&r) == -32700);
    done(&r);

    r = post("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}]", NULL);
    CHECK(r.status == 400 && error_code(&r) == -32600);
    done(&r);

    r = post("{\"jsonrpc\":\"2.0\",\"id\":\"s1\",\"method\":\"nope\"}", NULL);
    CHECK(r.status == 200 && error_code(&r) == -32601);   /* legacy: 200, not 404 */
    CHECK(strcmp(path(r.json, "id", NULL)->valuestring, "s1") == 0);
    done(&r);

    r = modern("tools/call", "engage", "{}");
    CHECK(r.status == 200 && error_code(&r) == -32602);
    done(&r);
}

static void test_state(void)
{
    fake_reset();
    fake.released = true;
    fake.mode = EYE_MODE_CALIBRATION;
    fake.angle[EYE_TL] = 90;
    resp_t r = modern("tools/call", "get_state", "{}");
    CHECK(r.status == 200 && !tool_is_error(&r));
    const cJSON *s = path(r.json, "result", "structuredContent");
    CHECK(cJSON_IsTrue(path(s, "released", NULL)));
    CHECK(strcmp(path(s, "mode", NULL)->valuestring, "calibration") == 0);
    CHECK(cJSON_IsNull(path(s, "animation", NULL)));
    CHECK(fabs(path(path(s, "gaze", NULL), "lr", NULL)->valuedouble - 0.5) < 1e-6);
    const cJSON *lr = cJSON_GetArrayItem(path(s, "servos", NULL), EYE_LR);
    CHECK(cJSON_IsNull(path(lr, "commanded_deg", NULL)));   /* never written: unknown, not 0 */
    CHECK(strstr(tool_text(&r), "\"released\":true") != NULL);
    done(&r);
}

static void test_motion_gating(void)
{
    fake_reset();
    fake.released = true;
    const char *calls[][2] = { { "look", "{\"lr\":0.2}" }, { "blink", "{}" },
                               { "play_animation", "{\"name\":\"look\"}" }, { "set_mode", "{\"mode\":\"auto\"}" } };
    for (size_t i = 0; i < 4; i++) {
        resp_t r = modern("tools/call", calls[i][0], calls[i][1]);
        CHECK(r.status == 200 && tool_is_error(&r) && strstr(tool_text(&r), "released"));
        done(&r);
    }
    fake.released = false;
    fake.mode = EYE_MODE_CALIBRATION;
    for (size_t i = 0; i < 4; i++) {
        resp_t r = modern("tools/call", calls[i][0], calls[i][1]);
        CHECK(tool_is_error(&r) && strstr(tool_text(&r), "calibration"));
        done(&r);
    }
    fake.mode = EYE_MODE_STANDBY;
    for (size_t i = 0; i < 4; i++) {
        resp_t r = modern("tools/call", calls[i][0], calls[i][1]);
        CHECK(tool_is_error(&r) && strstr(tool_text(&r), "standby"));
        done(&r);
    }
    CHECK(fake.looks == 0 && fake.blinks == 0 && fake.playing == NULL && fake.set_mode_calls == 0);

    /* The stops are never gated. */
    resp_t r = modern("tools/call", "stop_animation", "{}");
    CHECK(!tool_is_error(&r) && fake.anim_stops == 1);
    done(&r);
    r = modern("tools/call", "release", "{}");
    CHECK(!tool_is_error(&r) && fake.released && fake.release_calls == 1);
    done(&r);
}

static void test_look_maps_units_through_limits(void)
{
    fake_reset();
    resp_t r = modern("tools/call", "look", "{\"lr\":0,\"ud\":1}");
    CHECK(!tool_is_error(&r));
    CHECK(fabs(fake.lr - 42) < 1e-3 && fabs(fake.ud - 140) < 1e-3);
    CHECK(fake.mode == EYE_MODE_MANUAL);
    done(&r);

    r = modern("tools/call", "look", "{\"ud\":0.5}");   /* lr omitted: kept */
    CHECK(!tool_is_error(&r) && fabs(fake.lr - 42) < 1e-3 && fabs(fake.ud - 90) < 1e-3);
    done(&r);

    int before = fake.looks;
    const char *bad[] = { "{\"lr\":1.5}", "{\"lr\":-0.1}", "{\"lr\":\"left\"}", "{}" };
    for (size_t i = 0; i < 4; i++) {
        r = modern("tools/call", "look", bad[i]);
        CHECK(tool_is_error(&r));
        done(&r);
    }
    CHECK(fake.looks == before);
}

static void test_animation_and_mode_args(void)
{
    fake_reset();
    resp_t r = modern("tools/call", "play_animation", "{\"name\":\"roll\",\"repeat\":3}");
    CHECK(!tool_is_error(&r) && fake.last_repeat == 3 && strcmp(fake.playing, "roll") == 0);
    done(&r);

    const char *bad[] = { "{\"name\":\"dance\"}", "{\"name\":\"roll\",\"repeat\":-1}",
                          "{\"name\":\"roll\",\"repeat\":11}", "{\"name\":\"roll\",\"repeat\":1.5}", "{}" };
    for (size_t i = 0; i < 5; i++) {
        r = modern("tools/call", "play_animation", bad[i]);
        CHECK(tool_is_error(&r));
        done(&r);
    }

    r = modern("tools/call", "set_mode", "{\"mode\":\"calibration\"}");
    CHECK(tool_is_error(&r) && fake.mode == EYE_MODE_AUTO);
    done(&r);
    r = modern("tools/call", "set_mode", "{\"mode\":\"standby\"}");
    CHECK(tool_is_error(&r) && fake.mode == EYE_MODE_AUTO);
    done(&r);
    r = modern("tools/call", "set_mode", "{\"mode\":\"tracking\"}");
    CHECK(!tool_is_error(&r) && fake.mode == EYE_MODE_TRACKING && strstr(tool_text(&r), "no vision"));
    done(&r);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "legacy initialize and list", test_legacy_initialize_and_list },
        { "modern discover and list", test_modern_discover_and_list },
        { "modern header validation", test_modern_header_validation },
        { "origin and malformed", test_origin_and_malformed },
        { "get_state", test_state },
        { "motion gating", test_motion_gating },
        { "look maps units through limits", test_look_maps_units_through_limits },
        { "animation and mode args", test_animation_and_mode_args },
    };
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        int before = s_failures;
        tests[i].fn();
        printf("%s %s\n", s_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("%d failure(s)\n", s_failures);
    return s_failures ? 1 : 0;
}
