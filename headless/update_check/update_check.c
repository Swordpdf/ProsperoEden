/*
 * ps5-native-app-boilerplate - Update check against the homebrew.page catalog.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See update_check.h. This file is C11 and also compiles as C++.
 *
 * What it reads comes from the network, so nothing here trusts it: every
 * length is checked, strings are copied only into buffers that hold them, and
 * nesting is bounded. A value that doesn't fit is an error, never a truncation.
 */
#include "update_check.h"

#include <string.h>

/* Content versions: NN.NNN.NNN, compared as three numbers. */

int update_check_version_parse(const char *text, unsigned parts[3])
{
    static const char shape[] = "dd.ddd.ddd";
    unsigned values[3] = {0, 0, 0};
    unsigned field = 0;
    size_t i;
    if (text == NULL)
        return 0;
    for (i = 0; shape[i] != '\0'; ++i)
    {
        const char c = text[i];
        if (shape[i] == '.')
        {
            if (c != '.')
                return 0;
            ++field;
        }
        else
        {
            if (c < '0' || c > '9')
                return 0;
            values[field] = values[field] * 10u + (unsigned)(c - '0');
        }
    }
    if (text[i] != '\0')
        return 0;
    if (parts != NULL)
    {
        parts[0] = values[0];
        parts[1] = values[1];
        parts[2] = values[2];
    }
    return 1;
}

int update_check_version_compare(const char *left, const char *right, int *comparable)
{
    unsigned a[3];
    unsigned b[3];
    int i;
    const int ok = update_check_version_parse(left, a) && update_check_version_parse(right, b);
    if (comparable != NULL)
        *comparable = ok;
    if (!ok)
        return 0;
    for (i = 0; i < 3; ++i)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static int is_title_id(const char *text)
{
    size_t i;
    if (text == NULL)
        return 0;
    for (i = 0; i < 9; ++i)
    {
        const char c = text[i];
        const int ok = i < 4 ? (c >= 'A' && c <= 'Z') : (c >= '0' && c <= '9');
        if (!ok)
            return 0;
    }
    return text[9] == '\0';
}

const char *update_check_api(int origin)
{
    return origin == 0 ? UPDATE_CHECK_API : origin == 1 ? UPDATE_CHECK_MIRROR_API : NULL;
}

size_t update_check_url_at(char *out, size_t size, const char *title_id, int origin)
{
    static const char folder[] = "apps/";
    static const char suffix[] = ".json";
    const char *api = update_check_api(origin);
    size_t prefix;
    size_t length;
    if (api == NULL || out == NULL || title_id == NULL || !is_title_id(title_id))
        return 0;
    prefix = strlen(api) + sizeof(folder) - 1;
    length = prefix + 9 + sizeof(suffix) - 1;
    if (size <= length)
        return 0;
    memcpy(out, api, strlen(api));
    memcpy(out + strlen(api), folder, sizeof(folder) - 1);
    memcpy(out + prefix, title_id, 9);
    memcpy(out + prefix + 9, suffix, sizeof(suffix));
    return length;
}

size_t update_check_url(char *out, size_t size, const char *title_id)
{
    return update_check_url_at(out, size, title_id, 0);
}

/* A reader for one string value of a top-level JSON object. */

typedef struct json_cursor
{
    const char *at;
    const char *end;
} json_cursor;

static void json_skip_space(json_cursor *c)
{
    while (c->at < c->end && (*c->at == ' ' || *c->at == '\t' || *c->at == '\n' || *c->at == '\r'))
        ++c->at;
}

static int json_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Reads a string at the cursor. With `out`, copies it (UTF-8) and fails when it doesn't fit;
 * without, only skips it. Returns 1 on success. */
static int json_string(json_cursor *c, char *out, size_t size)
{
    size_t used = 0;
    if (c->at >= c->end || *c->at != '"')
        return 0;
    ++c->at;
    while (c->at < c->end)
    {
        unsigned code = (unsigned char)*c->at++;
        char encoded[3] = {0, 0, 0};
        size_t count = 1;
        size_t i;
        int escaped_code_point = 0; /* bytes of the text itself are already UTF-8 */
        if (code == '"')
        {
            if (out != NULL)
                out[used] = '\0';
            return 1;
        }
        if (code < 0x20)
            return 0;
        if (code == '\\')
        {
            if (c->at >= c->end)
                return 0;
            code = (unsigned char)*c->at++;
            switch (code)
            {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                code = '\b';
                break;
            case 'f':
                code = '\f';
                break;
            case 'n':
                code = '\n';
                break;
            case 'r':
                code = '\r';
                break;
            case 't':
                code = '\t';
                break;
            case 'u':
                if (c->end - c->at < 4)
                    return 0;
                code = 0;
                for (i = 0; i < 4; ++i)
                {
                    const int digit = json_hex(c->at[i]);
                    if (digit < 0)
                        return 0;
                    code = code * 16u + (unsigned)digit;
                }
                c->at += 4;
                if (code >= 0xD800u && code <= 0xDFFFu)
                    code = '?'; /* surrogate halves: nothing this reader needs */
                escaped_code_point = 1;
                break;
            default:
                return 0;
            }
        }
        if (!escaped_code_point || code < 0x80u)
        {
            encoded[0] = (char)code;
        }
        else if (code < 0x800u)
        {
            encoded[0] = (char)(0xC0u | (code >> 6));
            encoded[1] = (char)(0x80u | (code & 0x3Fu));
            count = 2;
        }
        else
        {
            encoded[0] = (char)(0xE0u | (code >> 12));
            encoded[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
            encoded[2] = (char)(0x80u | (code & 0x3Fu));
            count = 3;
        }
        if (out != NULL)
        {
            if (code == 0)
                return 0; /* These APIs return C strings; embedded NUL would truncate identity fields. */
            if (used + count >= size)
                return 0;
            for (i = 0; i < count; ++i)
                out[used + i] = encoded[i];
        }
        used += count;
    }
    return 0;
}

/* Skips one value of any type. Returns 1 on success. */
static int json_skip_value(json_cursor *c)
{
    int depth = 0;
    if (c->at >= c->end)
        return 0;
    if (*c->at == '"')
        return json_string(c, NULL, 0);
    if (*c->at != '{' && *c->at != '[')
    {
        const char *start = c->at;
        while (c->at < c->end && *c->at != ',' && *c->at != '}' && *c->at != ']' && *c->at != ' ' &&
               *c->at != '\t' && *c->at != '\n' && *c->at != '\r')
            ++c->at;
        return c->at > start;
    }
    while (c->at < c->end)
    {
        const char ch = *c->at;
        if (ch == '"')
        {
            if (!json_string(c, NULL, 0))
                return 0;
            continue;
        }
        ++c->at;
        if (ch == '{' || ch == '[')
        {
            if (++depth > 64)
                return 0;
        }
        else if ((ch == '}' || ch == ']') && --depth == 0)
        {
            return 1;
        }
    }
    return 0;
}

int update_check_json_value(const char *json, size_t length, const char *key,
                            const char **value, size_t *value_length)
{
    json_cursor c;
    int found = 0;
    if (!json || !key || !value || !value_length) return -1;
    c.at = json; c.end = json + length;
    json_skip_space(&c);
    if (c.at == c.end || *c.at++ != '{') return -1;
    json_skip_space(&c);
    while (c.at < c.end && *c.at != '}') {
        char name[128];
        const char *begin;
        if (!json_string(&c, name, sizeof(name))) return -1;
        json_skip_space(&c);
        if (c.at == c.end || *c.at++ != ':') return -1;
        json_skip_space(&c); begin = c.at;
        if (!json_skip_value(&c)) return -1;
        if (strcmp(name, key) == 0) {
            if (found) return -1;
            found = 1; *value = begin; *value_length = (size_t)(c.at - begin);
        }
        json_skip_space(&c);
        if (c.at < c.end && *c.at == '}') break;
        if (c.at == c.end || *c.at++ != ',') return -1;
        json_skip_space(&c);
        if (c.at == c.end || *c.at == '}') return -1;
    }
    if (c.at == c.end || *c.at++ != '}') return -1;
    json_skip_space(&c);
    return c.at == c.end ? found : -1;
}

int update_check_json_uint(const char *json, size_t length, const char *key, unsigned *out)
{
    const char *value = NULL;
    size_t size = 0, i;
    unsigned result = 0;
    int found = update_check_json_value(json, length, key, &value, &size);
    if (found != 1 || !out) return !out ? -1 : found;
    if (!size || (size > 1 && value[0] == '0')) return -1;
    for (i = 0; i < size; ++i) {
        unsigned digit;
        if (value[i] < '0' || value[i] > '9') return -1;
        digit = (unsigned)(value[i] - '0');
        if (result > (~0u - digit) / 10u) return -1;
        result = result * 10u + digit;
    }
    *out = result; return 1;
}

int update_check_json_array_string(const char *json, size_t length, size_t index, char *out, size_t size)
{
    json_cursor c;
    size_t at = 0;
    int found = 0;
    if (!json || !out || !size) return -1;
    c.at = json; c.end = json + length; json_skip_space(&c);
    if (c.at == c.end || *c.at++ != '[') return -1;
    json_skip_space(&c);
    while (c.at < c.end && *c.at != ']') {
        if (!json_string(&c, at == index ? out : NULL, at == index ? size : 0)) return -1;
        if (at++ == index) found = 1;
        json_skip_space(&c);
        if (c.at < c.end && *c.at == ']') break;
        if (c.at == c.end || *c.at++ != ',') return -1;
        json_skip_space(&c);
        if (c.at == c.end || *c.at == ']') return -1;
    }
    if (c.at == c.end || *c.at++ != ']') return -1;
    json_skip_space(&c); return c.at == c.end ? found : -1;
}

int update_check_json_string(const char *json, size_t length, const char *key, char *out,
                             size_t size)
{
    json_cursor c;
    char name[64];
    if (json == NULL || key == NULL || out == NULL || size == 0)
        return -1;
    c.at = json;
    c.end = json + length;
    json_skip_space(&c);
    if (c.at >= c.end || *c.at != '{')
        return -1;
    ++c.at;
    for (;;)
    {
        int matches;
        json_skip_space(&c);
        if (c.at < c.end && *c.at == '}')
            return 0;
        /* A name too long for `name` can't be the one asked for; it is skipped whole. */
        {
            json_cursor probe = c;
            if (json_string(&probe, name, sizeof(name)))
            {
                c = probe;
                matches = strcmp(name, key) == 0;
            }
            else if (json_string(&c, NULL, 0))
            {
                matches = 0;
            }
            else
            {
                return -1;
            }
        }
        json_skip_space(&c);
        if (c.at >= c.end || *c.at != ':')
            return -1;
        ++c.at;
        json_skip_space(&c);
        if (matches)
        {
            if (c.at < c.end && *c.at == '"')
                return json_string(&c, out, size) ? 1 : -1;
            if (c.end - c.at >= 4 && memcmp(c.at, "null", 4) == 0)
                return 2;
            return -1;
        }
        if (!json_skip_value(&c))
            return -1;
        json_skip_space(&c);
        if (c.at < c.end && *c.at == ',')
        {
            ++c.at;
            continue;
        }
        if (c.at < c.end && *c.at == '}')
            return 0;
        return -1;
    }
}

/* 1 when the text is one whole JSON object and nothing else. An answer cut short in transit
 * can hold complete-looking fields; it is refused as a whole rather than half-trusted. */
static int json_object_is_complete(const char *json, size_t length)
{
    json_cursor c;
    if (json == NULL)
        return 0;
    c.at = json;
    c.end = json + length;
    json_skip_space(&c);
    if (c.at >= c.end || *c.at != '{')
        return 0;
    ++c.at;
    json_skip_space(&c);
    if (c.at < c.end && *c.at == '}')
    {
        ++c.at;
    }
    else
    {
        for (;;)
        {
            json_skip_space(&c);
            if (!json_string(&c, NULL, 0))
                return 0;
            json_skip_space(&c);
            if (c.at >= c.end || *c.at != ':')
                return 0;
            ++c.at;
            json_skip_space(&c);
            if (!json_skip_value(&c))
                return 0;
            json_skip_space(&c);
            if (c.at >= c.end)
                return 0;
            if (*c.at == ',')
            {
                ++c.at;
                continue;
            }
            if (*c.at != '}')
                return 0;
            ++c.at;
            break;
        }
    }
    json_skip_space(&c);
    return c.at == c.end;
}

/* The decision. */

static void result_reset(update_check_result *result, const char *installed)
{
    memset(result, 0, sizeof(*result));
    result->state = UPDATE_CHECK_UNKNOWN;
    result->reason = UPDATE_CHECK_OK;
    if (installed != NULL && strlen(installed) < sizeof(result->installed))
        memcpy(result->installed, installed, strlen(installed) + 1);
}

void update_check_evaluate(const char *json, size_t length, const char *installed,
                           update_check_result *result)
{
    char status[24];
    int found;
    int comparable = 0;
    int order;
    if (result == NULL)
        return;
    {
        const int http_status = result->http_status;
        result_reset(result, installed);
        result->http_status = http_status;
    }
    if (!update_check_version_parse(installed, NULL))
    {
        result->reason = UPDATE_CHECK_BAD_INSTALLED_VERSION;
        return;
    }
    if (!json_object_is_complete(json, length) ||
        update_check_json_string(json, length, "status", status, sizeof(status)) != 1)
    {
        result->reason = UPDATE_CHECK_BAD_RESPONSE;
        return;
    }
    if (strcmp(status, "available") != 0)
    {
        result->reason = UPDATE_CHECK_NOT_AVAILABLE;
        return;
    }
    found = update_check_json_string(json, length, "content_version", result->available,
                                     sizeof(result->available));
    if (found == 0 || found == 2)
    {
        result->available[0] = '\0';
        result->reason = UPDATE_CHECK_NO_CATALOG_VERSION;
        return;
    }
    if (found != 1)
    {
        result->available[0] = '\0';
        result->reason = UPDATE_CHECK_BAD_RESPONSE;
        return;
    }
    order = update_check_version_compare(result->available, installed, &comparable);
    if (!comparable)
    {
        result->available[0] = '\0';
        result->reason = UPDATE_CHECK_BAD_RESPONSE;
        return;
    }
    /* For display only; a missing or oversized value is left empty. */
    if (update_check_json_string(json, length, "version", result->version,
                                 sizeof(result->version)) != 1)
        result->version[0] = '\0';
    if (update_check_json_string(json, length, "page", result->page, sizeof(result->page)) != 1)
        result->page[0] = '\0';
    result->state = order > 0 ? UPDATE_CHECK_AVAILABLE : UPDATE_CHECK_UP_TO_DATE;
}

/* One place asked: its answer, or why there is none. */
static void run_at(update_check_fetch_fn fetch, const char *title_id, const char *installed,
                   update_check_result *result, int origin)
{
    static char body[UPDATE_CHECK_MAX_RESPONSE];
    char url[128];
    char agent[48];
    size_t length = 0;
    int status = 0;
    int code;
    result_reset(result, installed);
    result->origin = origin;
    if (fetch == NULL || update_check_url_at(url, sizeof(url), title_id, origin) == 0)
    {
        result->reason = UPDATE_CHECK_BAD_ARGUMENT;
        return;
    }
    if (!update_check_version_parse(installed, NULL))
    {
        result->reason = UPDATE_CHECK_BAD_INSTALLED_VERSION;
        return;
    }
    /* The catalog's CDN refuses some libraries' default agents; name the app instead. */
    memcpy(agent, "homebrew-update-check/1 (", 25);
    memcpy(agent + 25, title_id, 9);
    memcpy(agent + 34, ")", 2);
    code = fetch(url, agent, body, sizeof(body), &length, &status);
    result->http_status = status;
    if (code == UPDATE_CHECK_FETCH_TOO_LARGE || length > sizeof(body))
    {
        result->reason = UPDATE_CHECK_TOO_LARGE;
        return;
    }
    if (code != 0)
    {
        result->reason = UPDATE_CHECK_NETWORK;
        result->platform_error = code;
        return;
    }
    if (status == 404)
    {
        result->reason = UPDATE_CHECK_NOT_LISTED;
        return;
    }
    if (status != 200)
    {
        result->reason = UPDATE_CHECK_HTTP_STATUS;
        return;
    }
    update_check_evaluate(body, length, installed, result);
    result->origin = origin;
}

/* Whether a place gave no usable answer: unreachable, an error status, or something that isn't
 * the catalog's JSON (a network's block page answers 200 with a page of its own). */
static int no_answer(const update_check_result *result)
{
    return result->reason == UPDATE_CHECK_NETWORK || result->reason == UPDATE_CHECK_HTTP_STATUS ||
           result->reason == UPDATE_CHECK_TOO_LARGE || result->reason == UPDATE_CHECK_BAD_RESPONSE;
}

void update_check_run_with(update_check_fetch_fn fetch, const char *title_id, const char *installed,
                           update_check_result *result)
{
    update_check_result mirrored;
    if (result == NULL)
        return;
    /* The catalog's own site first; its mirror only when the site gives no usable answer. A
     * failure of both is reported as the site's. */
    run_at(fetch, title_id, installed, result, 0);
    if (!no_answer(result))
        return;
    run_at(fetch, title_id, installed, &mirrored, 1);
    if (!no_answer(&mirrored))
        *result = mirrored;
}

const char *update_check_reason_text(update_check_reason reason)
{
    switch (reason)
    {
    case UPDATE_CHECK_OK:
        return "ok";
    case UPDATE_CHECK_BAD_ARGUMENT:
        return "bad-argument";
    case UPDATE_CHECK_BAD_INSTALLED_VERSION:
        return "bad-installed-version";
    case UPDATE_CHECK_NETWORK:
        return "network";
    case UPDATE_CHECK_NOT_LISTED:
        return "not-listed";
    case UPDATE_CHECK_HTTP_STATUS:
        return "http-status";
    case UPDATE_CHECK_TOO_LARGE:
        return "too-large";
    case UPDATE_CHECK_BAD_RESPONSE:
        return "bad-response";
    case UPDATE_CHECK_NOT_AVAILABLE:
        return "not-available";
    case UPDATE_CHECK_NO_CATALOG_VERSION:
        return "no-catalog-version";
    }
    return "unknown";
}

#ifndef UPDATE_CHECK_NO_NETWORK

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif
    int sceKernelOpen(const char *path, int flags, int mode);
    int64_t sceKernelRead(int descriptor, void *buffer, size_t length);
    int sceKernelClose(int descriptor);
#ifdef __cplusplus
}
#endif

#ifndef UPDATE_CHECK_USE_SCEHTTP

/* libcurl with OpenSSL, verifying against the console's certificate list. console_curl.c makes
 * PacBrew's archives work in a native title; the options are the ones found necessary on the
 * console. A fresh handle per check: the check runs once per launch. */

#include "console_curl.h"

#include <curl/curl.h>
#include <pthread.h>

enum
{
    update_check_timeout_ms = 5000
};

typedef struct update_check_sink
{
    char *body;
    size_t capacity;
    size_t used;
    int overflow;
} update_check_sink;

static pthread_once_t update_check_curl_once = PTHREAD_ONCE_INIT;
static CURLcode update_check_curl_started = CURLE_FAILED_INIT;

static void update_check_curl_start(void)
{
    update_check_curl_started = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t update_check_on_body(char *data, size_t size, size_t count, void *user)
{
    update_check_sink *sink = (update_check_sink *)user;
    const size_t bytes = size * count;
    if (bytes > sink->capacity - sink->used)
    {
        sink->overflow = 1;
        return 0; /* anything but `bytes` ends the transfer */
    }
    memcpy(sink->body + sink->used, data, bytes);
    sink->used += bytes;
    return bytes;
}

static int update_check_curl_error(CURLcode code)
{
    return -(10000 + (int)code);
}

#ifdef UPDATE_CHECK_CURL_TRACE
/* Diagnostics: libcurl's own account of each step, in the kernel log with the time since the
 * process started. Headers and bodies are left out. */
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif
    int sceKernelDebugOutText(int channel, const char *text);
    uint64_t sceKernelGetProcessTime(void);
#ifdef __cplusplus
}
#endif

static int update_check_on_trace(CURL *easy, curl_infotype type, char *data, size_t size,
                                 void *user)
{
    char line[256];
    (void)easy;
    (void)user;
    if (type != CURLINFO_TEXT)
        return 0;
    while (size > 0 && (data[size - 1] == '\n' || data[size - 1] == '\r'))
        --size;
    (void)snprintf(line, sizeof(line), "UPDATE-CHECK-CURL: %llu ms %.*s\n",
                   (unsigned long long)(sceKernelGetProcessTime() / 1000u),
                   (int)(size < 200 ? size : 200), data);
    (void)sceKernelDebugOutText(0, line);
    return 0;
}
#endif

static int update_check_fetch(const char *url, const char *user_agent, char *body, size_t capacity,
                              size_t *length, int *http_status)
{
    update_check_sink sink = {body, capacity, 0, 0};
    long status = 0;
    *length = 0;
    *http_status = 0;

    (void)pthread_once(&update_check_curl_once, update_check_curl_start);
    if (update_check_curl_started != CURLE_OK)
        return update_check_curl_error(update_check_curl_started);
    CURL *easy = curl_easy_init();
    if (easy == NULL)
        return update_check_curl_error(CURLE_FAILED_INIT);

    console_curl_setup(easy); /* no signals, the console's CA list, non-blocking sockets */
    (void)curl_easy_setopt(easy, CURLOPT_URL, url);
    (void)curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent);
    (void)curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    (void)curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    (void)curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    (void)curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)update_check_timeout_ms);
    (void)curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 3L * update_check_timeout_ms);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, update_check_on_body);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
#ifdef UPDATE_CHECK_CURL_TRACE
    (void)curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, update_check_on_trace);
    (void)curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
#endif

    const CURLcode code = curl_easy_perform(easy);
    (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);
    *http_status = (int)status;
    if (sink.overflow)
        return UPDATE_CHECK_FETCH_TOO_LARGE;
    if (code != CURLE_OK)
        return update_check_curl_error(code);
    *length = sink.used;
    return 0;
}

#else /* UPDATE_CHECK_USE_SCEHTTP */

/* The console's HTTPS: the system sceHttp and sceSsl services, with the system's certificate
 * store. The sequence and option values follow apps where they are proven on hardware. Fails
 * with 0x8095f00c on every public site once the app is elevated. */

#ifdef __cplusplus
extern "C"
{
#endif
    int sceNetPoolCreate(const char *name, int size, int flags);
    int sceNetPoolDestroy(int pool);
    int sceSslInit(size_t pool_size);
    int sceSslTerm(int context);
    int sceHttpInit(int net_pool, int ssl_context, size_t pool_size);
    int sceHttpTerm(int context);
    int sceHttpCreateTemplate(int context, const char *user_agent, int version, int auto_proxy);
    int sceHttpDeleteTemplate(int template_id);
    int sceHttpCreateConnectionWithURL(int template_id, const char *url, int keep_alive);
    int sceHttpDeleteConnection(int connection);
    int sceHttpCreateRequestWithURL(int connection, int method, const char *url,
                                    uint64_t content_length);
    int sceHttpDeleteRequest(int request);
    int sceHttpSetAutoRedirect(int id, int enabled);
    int sceHttpSetResolveTimeOut(int id, uint32_t usec);
    int sceHttpSetConnectTimeOut(int id, uint32_t usec);
    int sceHttpSetSendTimeOut(int id, uint32_t usec);
    int sceHttpSetRecvTimeOut(int id, uint32_t usec);
    int sceHttpsEnableOption(int id, uint32_t flags);
    int sceHttpSendRequest(int request, const void *data, size_t size);
    int sceHttpGetStatusCode(int request, int *status);
    int sceHttpReadData(int request, void *data, size_t size);
#ifdef __cplusplus
}
#endif

enum
{
    update_check_net_pool = 1024 * 1024,
    update_check_ssl_pool = 304 * 1024,
    update_check_http_pool = 4 * 1024 * 1024,
    update_check_http_1_1 = 2,
    update_check_method_get = 0,
    update_check_timeout_usec = 5000000,
    /* server verify | name check | not-after | not-before | known CA | SNI */
    update_check_verify_flags = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80
};

static int update_check_fetch(const char *url, const char *user_agent, char *body, size_t capacity,
                              size_t *length, int *http_status)
{
    int pool = -1;
    int ssl = -1;
    int http = -1;
    int tmpl = -1;
    int connection = -1;
    int request = -1;
    int result;
    size_t used = 0;
    *length = 0;
    *http_status = 0;

    result = pool = sceNetPoolCreate("update_check", update_check_net_pool, 0);
    if (result >= 0)
        result = ssl = sceSslInit(update_check_ssl_pool);
    if (result >= 0)
        result = http = sceHttpInit(pool, ssl, update_check_http_pool);
    if (result >= 0)
        result = tmpl = sceHttpCreateTemplate(http, user_agent, update_check_http_1_1, 0);
    if (result >= 0)
        result = sceHttpSetAutoRedirect(tmpl, 0);
    if (result >= 0)
        result = sceHttpSetResolveTimeOut(tmpl, update_check_timeout_usec);
    if (result >= 0)
        result = sceHttpSetConnectTimeOut(tmpl, update_check_timeout_usec);
    if (result >= 0)
        result = sceHttpSetSendTimeOut(tmpl, update_check_timeout_usec);
    if (result >= 0)
        result = sceHttpSetRecvTimeOut(tmpl, update_check_timeout_usec);
    if (result >= 0)
        result = sceHttpsEnableOption(tmpl, update_check_verify_flags);
    if (result >= 0)
        result = connection = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (result >= 0)
        result = request = sceHttpCreateRequestWithURL(connection, update_check_method_get, url, 0);
    if (result >= 0)
        result = sceHttpSendRequest(request, NULL, 0);
    if (result >= 0)
        result = sceHttpGetStatusCode(request, http_status);
    while (result >= 0)
    {
        /* One spare byte tells an answer that fills the buffer from one that overflows it. */
        char spare;
        const int got = used < capacity ? sceHttpReadData(request, body + used, capacity - used)
                                        : sceHttpReadData(request, &spare, 1);
        if (got < 0)
        {
            result = got;
            break;
        }
        if (got == 0)
            break;
        if (used >= capacity)
        {
            result = UPDATE_CHECK_FETCH_TOO_LARGE;
            break;
        }
        used += (size_t)got;
    }
    if (request >= 0)
        (void)sceHttpDeleteRequest(request);
    if (connection >= 0)
        (void)sceHttpDeleteConnection(connection);
    if (tmpl >= 0)
        (void)sceHttpDeleteTemplate(tmpl);
    if (http >= 0)
        (void)sceHttpTerm(http);
    if (ssl >= 0)
        (void)sceSslTerm(ssl);
    if (pool >= 0)
        (void)sceNetPoolDestroy(pool);
    if (result == UPDATE_CHECK_FETCH_TOO_LARGE)
        return UPDATE_CHECK_FETCH_TOO_LARGE;
    if (result < 0)
        return result;
    *length = used;
    return 0;
}

#endif /* UPDATE_CHECK_USE_SCEHTTP */

void update_check_run(const char *title_id, const char *installed, update_check_result *result)
{
    update_check_run_with(update_check_fetch, title_id, installed, result);
}

int update_check_read_param(const char *path, char title_id[10], char content_version[12])
{
    static char text[16384];
    size_t used = 0;
    const int descriptor = sceKernelOpen(path, 0, 0);
    if (descriptor < 0)
        return 0;
    while (used < sizeof(text))
    {
        const int64_t got = sceKernelRead(descriptor, text + used, sizeof(text) - used);
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    (void)sceKernelClose(descriptor);
    return used < sizeof(text) &&
           update_check_json_string(text, used, "titleId", title_id, 10) == 1 &&
           update_check_json_string(text, used, "contentVersion", content_version, 12) == 1;
}

void update_check_run_self(update_check_result *result)
{
    char title_id[10];
    char content_version[12];
    if (result == NULL)
        return;
    if (!update_check_read_param("/app0/sce_sys/param.json", title_id, content_version))
    {
        result_reset(result, NULL);
        result->reason = UPDATE_CHECK_BAD_ARGUMENT;
        return;
    }
    update_check_run(title_id, content_version, result);
}

#endif /* UPDATE_CHECK_NO_NETWORK */
