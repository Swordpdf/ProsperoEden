/*
 * ps5-native-app-boilerplate - Update check against the homebrew.page catalog.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Lets an app listed on https://homebrew.page tell its user that a newer
 * release exists. Copy update_check.h, update_check.c, console_curl.h and
 * console_curl.c into your project; they depend on nothing else in this
 * repository.
 *
 * Transport: libcurl with OpenSSL and the console's certificate list, which
 * works sandboxed and elevated. Add to the Makefile:
 *
 *     PACBREW_PACKAGES += libcurl
 *     APP_WRAP_SYMBOLS += fcntl
 *
 * Define UPDATE_CHECK_USE_SCEHTTP to use the system's sceHttp instead (no
 * libcurl, no console_curl.c; sandboxed apps only: sceSsl rejects public
 * certificates once an app is elevated). Define UPDATE_CHECK_NO_NETWORK to
 * build only the parsing and decisions, for host tests.
 *
 *     update_check_result result;
 *     update_check_run_self(&result);            // on a worker thread
 *     if (result.state == UPDATE_CHECK_AVAILABLE)
 *         show("Update available: %s", result.version);
 *
 * The check is one HTTPS GET of the app's own file in the catalog's store API
 * (https://homebrew.page/api/v1/apps/<TITLEID>.json) and a comparison of its
 * content_version with the contentVersion of the running app's param.json.
 * It downloads nothing else, installs nothing, and needs no elevation.
 *
 * Every failure (no network, the app isn't listed, an answer that can't be
 * read) gives UPDATE_CHECK_UNKNOWN. Show nothing in that case.
 */
#ifndef PS5_BOILERPLATE_UPDATE_CHECK_H
#define PS5_BOILERPLATE_UPDATE_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define UPDATE_CHECK_HOST "homebrew.page"
/* The catalog's store API is published in two places: on its own site, and as a mirror for
 * networks that block that site. They hold the same answers. Each place has its own signed
 * manifest, because icon addresses inside the files name the place they are served from. */
#define UPDATE_CHECK_API "https://" UPDATE_CHECK_HOST "/api/v1/"
#define UPDATE_CHECK_MIRROR_API "https://blackbearreloaded.github.io/ps5-homebrew-catalog/api/v1/"
#define UPDATE_CHECK_ORIGINS 2
#define UPDATE_CHECK_MAX_RESPONSE 65536u

    typedef enum update_check_state
    {
        UPDATE_CHECK_UNKNOWN = 0,    /* nothing can be said; show nothing */
        UPDATE_CHECK_UP_TO_DATE = 1, /* the catalog has nothing newer */
        UPDATE_CHECK_AVAILABLE = 2   /* the catalog lists a higher content version */
    } update_check_state;

    typedef enum update_check_reason
    {
        UPDATE_CHECK_OK = 0,
        UPDATE_CHECK_BAD_ARGUMENT = 1,          /* title ID or buffer not usable */
        UPDATE_CHECK_BAD_INSTALLED_VERSION = 2, /* not NN.NNN.NNN */
        UPDATE_CHECK_NETWORK = 3,               /* the request failed; see platform_error */
        UPDATE_CHECK_NOT_LISTED = 4,            /* HTTP 404: the app isn't in the catalog */
        UPDATE_CHECK_HTTP_STATUS = 5,           /* another HTTP status; see http_status */
        UPDATE_CHECK_TOO_LARGE = 6,             /* answer over UPDATE_CHECK_MAX_RESPONSE */
        UPDATE_CHECK_BAD_RESPONSE = 7,          /* not the JSON the API documents */
        UPDATE_CHECK_NOT_AVAILABLE = 8,         /* listed as coming soon: no release yet */
        UPDATE_CHECK_NO_CATALOG_VERSION = 9     /* the catalog knows no content version */
    } update_check_reason;

    typedef struct update_check_result
    {
        update_check_state state;
        update_check_reason reason;
        int http_status;    /* 0 when no answer arrived */
        int platform_error; /* the first failing system call's code, or 0 */
        char installed[12]; /* the running app's contentVersion */
        char available[12]; /* the catalog's content_version, when known */
        char version[40];   /* the release's name for display, e.g. "0.11.0" */
        char page[160];     /* the app's page on homebrew.page */
        int origin;         /* which place answered: 0 the catalog's site, 1 its mirror */
    } update_check_result;

    /* Fetches `url` with `user_agent` into `body` (at most `capacity` bytes), storing the byte
     * count and the HTTP status. Returns 0 when an answer arrived, a negative code otherwise,
     * and UPDATE_CHECK_FETCH_TOO_LARGE when the answer doesn't fit. */
    typedef int (*update_check_fetch_fn)(const char *url, const char *user_agent, char *body,
                                         size_t capacity, size_t *length, int *http_status);
#define UPDATE_CHECK_FETCH_TOO_LARGE 1

    /* The pieces, usable and testable without a console. */

    /* 1 when `text` is a content version (NN.NNN.NNN); `parts` then holds its three numbers. */
    int update_check_version_parse(const char *text, unsigned parts[3]);

    /* Compares two content versions: negative, zero or positive like strcmp.
     * Returns 0 and sets *comparable to 0 when either isn't a content version. */
    int update_check_version_compare(const char *left, const char *right, int *comparable);

    /* Writes the API address of `title_id` into `out`. Returns its length, or 0 when the title ID
     * isn't four capital letters and five digits or `out` is too small. */
    size_t update_check_url(char *out, size_t size, const char *title_id);

    /* Copies the string value of a top-level `key` of a JSON object into `out`.
     * 1: copied. 0: no such key. 2: the value is null. -1: malformed, not a string, or too long. */
    int update_check_json_string(const char *json, size_t length, const char *key, char *out,
                                 size_t size);

    /* Bounded views of top-level values, unsigned integers and string-array elements.
     * 1: found, 0: absent/end, -1: malformed or unsupported. Views borrow the input. */
    int update_check_json_value(const char *json, size_t length, const char *key,
                                const char **value, size_t *value_length);
    int update_check_json_uint(const char *json, size_t length, const char *key, unsigned *out);
    int update_check_json_array_string(const char *json, size_t length, size_t index,
                                       char *out, size_t size);

    /* Decides from an app's API file and the installed content version. Never fails: every
     * problem is a result with state UPDATE_CHECK_UNKNOWN and a reason. */
    void update_check_evaluate(const char *json, size_t length, const char *installed,
                               update_check_result *result);

    /* The whole check with a transport of your own (tests use this; so can an app that
     * already has an HTTP client). Blocking. */
    /* The API's address at a place: 0 the catalog's site, 1 its mirror; NULL for any other. */
    const char *update_check_api(int origin);
    /* As update_check_url, for the place `origin`. */
    size_t update_check_url_at(char *out, size_t size, const char *title_id, int origin);
    void update_check_run_with(update_check_fetch_fn fetch, const char *title_id,
                               const char *installed, update_check_result *result);

    /* A short English word for a reason, for logs. */
    const char *update_check_reason_text(update_check_reason reason);

#ifndef UPDATE_CHECK_NO_NETWORK
    /* The whole check over HTTPS (libcurl, or sceHttp with UPDATE_CHECK_USE_SCEHTTP), with
     * certificate verification. A failed request sets platform_error: -(10000 + CURLcode) with
     * libcurl, the sceHttp/sceSsl code otherwise. Blocking for up to a few seconds per phase:
     * call it from a worker thread, at most once per launch, and never make the app wait. */
    void update_check_run(const char *title_id, const char *installed, update_check_result *result);

    /* The same for the running app: the title ID and contentVersion are read from
     * /app0/sce_sys/param.json, so there is nothing to configure. */
    void update_check_run_self(update_check_result *result);

    /* Reads titleId and contentVersion from a param.json. 1 on success. */
    int update_check_read_param(const char *path, char title_id[10], char content_version[12]);
#endif

#ifdef __cplusplus
}
#endif

#endif
