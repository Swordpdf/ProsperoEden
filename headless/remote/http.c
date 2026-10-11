/*
 * ProsperoEden - HTTP for the download sources; see http.h.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "http.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifndef REMOTE_HTTP_HOST
#include "console_curl.h"
#endif

enum
{
    remote_http_connect_ms = 10000, /* covers the name lookup too */
    remote_http_stall_seconds = 30, /* a large file: given up when nothing arrives for this long */
    remote_http_redirects = 5
};

static pthread_once_t remote_http_once = PTHREAD_ONCE_INIT;
static CURLcode remote_http_started = CURLE_FAILED_INIT;

static void start_curl(void)
{
    remote_http_started = curl_global_init(CURL_GLOBAL_DEFAULT);
}

typedef struct transfer
{
    const remote_http_request *request;
    CURL *easy;
    int begun;
    int stopped;
} transfer;

static size_t on_body(char *data, size_t size, size_t count, void *user)
{
    transfer *t = (transfer *)user;
    const size_t bytes = size * count;
    if (!t->begun)
    {
        long code = 0;
        curl_off_t length = -1;
        t->begun = 1;
        (void)curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code);
        (void)curl_easy_getinfo(t->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        if (t->request->begin != NULL &&
            t->request->begin(t->request->user, (int)code, length > 0 ? (uint64_t)length : 0) != 1)
        {
            t->stopped = 1;
            return 0;
        }
    }
    if (t->request->sink(t->request->user, data, bytes) != 1)
    {
        t->stopped = 1;
        return 0;
    }
    return bytes;
}

static int on_progress(void *user, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    transfer *t = (transfer *)user;
    (void)total;
    (void)now;
    (void)up_total;
    (void)up_now;
    if (t->request->stop != NULL && t->request->stop(t->request->user))
    {
        t->stopped = 1;
        return 1;
    }
    return 0;
}

int remote_http_get(const remote_http_request *request, remote_http_result *result)
{
    return remote_http_run(request, result);
}

int remote_http_run(const remote_http_request *request, remote_http_result *result)
{
    struct curl_slist *headers = NULL;
    curl_mime *form = NULL;
    char line[600];
    char range[48];
    transfer t;
    long code = 0;
    curl_off_t length = -1;
    CURLcode done;
    memset(result, 0, sizeof(*result));
    (void)pthread_once(&remote_http_once, start_curl);
    if (remote_http_started != CURLE_OK)
    {
        result->curl_code = (int)remote_http_started;
        (void)snprintf(result->error, sizeof(result->error), "The network library did not start");
        return -1;
    }
    memset(&t, 0, sizeof(t));
    t.request = request;
    t.easy = curl_easy_init();
    if (t.easy == NULL)
    {
        result->curl_code = (int)CURLE_FAILED_INIT;
        (void)snprintf(result->error, sizeof(result->error), "The network library did not start");
        return -1;
    }
#ifndef REMOTE_HTTP_HOST
    console_curl_setup(t.easy); /* no signals, the console's CA list, non-blocking sockets */
#else
    (void)curl_easy_setopt(t.easy, CURLOPT_NOSIGNAL, 1L);
#endif
    (void)curl_easy_setopt(t.easy, CURLOPT_URL, request->url);
    (void)curl_easy_setopt(t.easy, CURLOPT_USERAGENT, "ProsperoEden/1");
    (void)curl_easy_setopt(t.easy, CURLOPT_PROTOCOLS_STR, request->https_only ? "https" : "http,https");
    (void)curl_easy_setopt(t.easy, CURLOPT_REDIR_PROTOCOLS_STR, request->https_only ? "https" : "http,https");
    /* A reverse proxy in front of a server may redirect; curl keeps the Authorization header to the
     * first host only (CURLOPT_UNRESTRICTED_AUTH stays off). */
    (void)curl_easy_setopt(t.easy, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(t.easy, CURLOPT_MAXREDIRS, (long)remote_http_redirects);
    (void)curl_easy_setopt(t.easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    (void)curl_easy_setopt(t.easy, CURLOPT_CONNECTTIMEOUT_MS, (long)remote_http_connect_ms);
    (void)curl_easy_setopt(t.easy, CURLOPT_BUFFERSIZE, 1024L * 1024L); /* fewer, larger pieces of a large file */
    if (!request->raw)
        (void)curl_easy_setopt(t.easy, CURLOPT_ACCEPT_ENCODING, ""); /* JSON lists compress well */
    if (request->timeout_ms > 0)
        (void)curl_easy_setopt(t.easy, CURLOPT_TIMEOUT_MS, request->timeout_ms);
    (void)curl_easy_setopt(t.easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    (void)curl_easy_setopt(t.easy, CURLOPT_LOW_SPEED_TIME, (long)remote_http_stall_seconds);
    if (request->authorization != NULL && request->authorization[0] != '\0')
    {
        (void)snprintf(line, sizeof(line), "Authorization: %s", request->authorization);
        headers = curl_slist_append(headers, line);
    }
    headers = curl_slist_append(headers, "Accept: application/json, */*");
    if (request->api_version != NULL)
    {
        (void)snprintf(line, sizeof(line), "api-version: %s", request->api_version);
        headers = curl_slist_append(headers, line);
    }
    if (request->body != NULL)
    {
        (void)snprintf(line, sizeof(line), "Content-Type: %s",
                       request->content_type != NULL ? request->content_type : "application/json");
        headers = curl_slist_append(headers, line);
        (void)curl_easy_setopt(t.easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_size);
        (void)curl_easy_setopt(t.easy, CURLOPT_COPYPOSTFIELDS, request->body);
    }
    else if (request->file_path != NULL)
    {
        curl_mimepart *part;
        form = curl_mime_init(t.easy);
        part = curl_mime_addpart(form);
        if (form == NULL || part == NULL ||
            curl_mime_name(part, request->file_field != NULL ? request->file_field : "file") != CURLE_OK ||
            curl_mime_filedata(part, request->file_path) != CURLE_OK ||
            curl_mime_filename(part, request->file_name) != CURLE_OK ||
            curl_mime_type(part, "application/octet-stream") != CURLE_OK)
        {
            curl_mime_free(form);
            curl_slist_free_all(headers);
            curl_easy_cleanup(t.easy);
            result->curl_code = (int)CURLE_READ_ERROR;
            (void)snprintf(result->error, sizeof(result->error), "The file to send cannot be read");
            return -1;
        }
        (void)curl_easy_setopt(t.easy, CURLOPT_MIMEPOST, form);
        headers = curl_slist_append(headers, "Expect:"); /* no 100-continue round trip first */
    }
    /* A body or a form makes it a POST already, and a redirect (a proxy's to https) then sends it
     * again as a POST with its body; a verb set by hand would go without the body. Only another
     * verb (PUT, DELETE) is set so. */
    if (request->method != NULL && strcmp(request->method, "POST") == 0 && request->body == NULL &&
        request->file_path == NULL)
    {
        /* A POST without a body is an empty one, not a GET. */
        (void)curl_easy_setopt(t.easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
        (void)curl_easy_setopt(t.easy, CURLOPT_COPYPOSTFIELDS, "");
    }
    if (request->body != NULL || request->file_path != NULL ||
        (request->method != NULL && strcmp(request->method, "POST") == 0))
        (void)curl_easy_setopt(t.easy, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);
    if (request->method != NULL && strcmp(request->method, "POST") != 0)
        (void)curl_easy_setopt(t.easy, CURLOPT_CUSTOMREQUEST, request->method);
    (void)curl_easy_setopt(t.easy, CURLOPT_HTTPHEADER, headers);
    if (request->resume_from > 0)
    {
        (void)snprintf(range, sizeof(range), "%llu-", (unsigned long long)request->resume_from);
        (void)curl_easy_setopt(t.easy, CURLOPT_RANGE, range);
    }
    (void)curl_easy_setopt(t.easy, CURLOPT_WRITEFUNCTION, on_body);
    (void)curl_easy_setopt(t.easy, CURLOPT_WRITEDATA, &t);
    (void)curl_easy_setopt(t.easy, CURLOPT_NOPROGRESS, 0L);
    (void)curl_easy_setopt(t.easy, CURLOPT_XFERINFOFUNCTION, on_progress);
    (void)curl_easy_setopt(t.easy, CURLOPT_XFERINFODATA, &t);

    done = curl_easy_perform(t.easy);
    (void)curl_easy_getinfo(t.easy, CURLINFO_RESPONSE_CODE, &code);
    (void)curl_easy_getinfo(t.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    curl_easy_cleanup(t.easy);
    curl_mime_free(form);
    curl_slist_free_all(headers);

    result->status = (int)code;
    result->length = length > 0 ? (uint64_t)length : 0;
    result->stopped = t.stopped;
    /* An answer without a body: the sink was never told its status. */
    if (done == CURLE_OK && !t.begun && request->begin != NULL)
        (void)request->begin(request->user, (int)code, 0);
    if (done == CURLE_OK)
        return 0;
    result->curl_code = (int)done;
    if (t.stopped)
        (void)snprintf(result->error, sizeof(result->error), "Stopped");
    else if (done == CURLE_COULDNT_RESOLVE_HOST)
        (void)snprintf(result->error, sizeof(result->error), "The server's name could not be found");
    else if (done == CURLE_COULDNT_CONNECT)
        (void)snprintf(result->error, sizeof(result->error), "The server did not answer");
    else if (done == CURLE_OPERATION_TIMEDOUT)
        (void)snprintf(result->error, sizeof(result->error), "The connection timed out");
    else if (done == CURLE_PEER_FAILED_VERIFICATION || done == CURLE_SSL_CONNECT_ERROR)
        (void)snprintf(result->error, sizeof(result->error), "Secure connection failed (%s)",
                       curl_easy_strerror(done));
    else
        (void)snprintf(result->error, sizeof(result->error), "%s", curl_easy_strerror(done));
    return -1;
}

static const char remote_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int remote_http_basic(const char *user, const char *password, char *out, size_t size)
{
    char plain[384];
    size_t length;
    size_t used = 0;
    size_t i;
    if (snprintf(plain, sizeof(plain), "%s:%s", user, password) >= (int)sizeof(plain))
        return -1;
    length = strlen(plain);
    if (size < 6 + (length + 2) / 3 * 4 + 1)
        return -1;
    memcpy(out, "Basic ", 6);
    used = 6;
    for (i = 0; i < length; i += 3)
    {
        const unsigned a = (unsigned char)plain[i];
        const unsigned b = i + 1 < length ? (unsigned char)plain[i + 1] : 0;
        const unsigned c = i + 2 < length ? (unsigned char)plain[i + 2] : 0;
        const unsigned triple = (a << 16) | (b << 8) | c;
        out[used++] = remote_base64[(triple >> 18) & 63];
        out[used++] = remote_base64[(triple >> 12) & 63];
        out[used++] = i + 1 < length ? remote_base64[(triple >> 6) & 63] : '=';
        out[used++] = i + 2 < length ? remote_base64[triple & 63] : '=';
    }
    out[used] = '\0';
    return 0;
}

int remote_http_escape(const char *segment, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; *segment != '\0'; ++segment)
    {
        const unsigned char c = (unsigned char)*segment;
        const int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (used + (plain ? 1 : 3) >= size)
            return -1;
        if (plain)
        {
            out[used++] = (char)c;
        }
        else
        {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
        }
    }
    if (used >= size)
        return -1;
    out[used] = '\0';
    return 0;
}
