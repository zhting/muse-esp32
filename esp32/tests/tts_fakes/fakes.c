/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pthread-backed FreeRTOS queues, stream buffers and tasks, the clock, and a
 * scripted HTTP server, for tests/muse_tts_harness.c.
 */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fake_http.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void esp_fill_random(void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ((unsigned char *)buf)[i] = (unsigned char)rand();
    }
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

static void deadline(struct timespec *ts, TickType_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

/* Waits on cv; false once wait (ms, or portMAX_DELAY) has run out. */
static bool wait_cv(pthread_cond_t *cv, pthread_mutex_t *mu, TickType_t wait, const struct timespec *until)
{
    if (wait == portMAX_DELAY) {
        pthread_cond_wait(cv, mu);
        return true;
    }
    return pthread_cond_timedwait(cv, mu, until) != ETIMEDOUT;
}

/* ---- Queues ---- */

struct fake_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned length, size, count, head;
    unsigned char *items;
};

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    struct fake_queue *q = calloc(1, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->length = length;
    q->size = item_size;
    q->items = calloc(length, item_size);
    return q;
}

void vQueueDelete(QueueHandle_t q)
{
    free(q->items);
    free(q);
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    struct timespec until;
    deadline(&until, wait);
    pthread_mutex_lock(&q->mu);
    while (q->count == q->length) {
        if (!wait || !wait_cv(&q->cv, &q->mu, wait, &until)) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
    }
    memcpy(q->items + ((q->head + q->count) % q->length) * q->size, item, q->size);
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    struct timespec until;
    deadline(&until, wait);
    pthread_mutex_lock(&q->mu);
    while (!q->count) {
        if (!wait || !wait_cv(&q->cv, &q->mu, wait, &until)) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
    }
    memcpy(item, q->items + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

/* ---- Stream buffers ---- */

struct fake_stream {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t size, head, count;
    unsigned char *data;
    int waiting;   /* tasks blocked in it: Reset must find none, as on FreeRTOS */
};

StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t trigger, uint32_t caps)
{
    (void)trigger;
    (void)caps;
    struct fake_stream *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->size = size;
    s->data = malloc(size);
    return s;
}

size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t wait)
{
    struct timespec until;
    deadline(&until, wait);
    pthread_mutex_lock(&s->mu);
    while (s->size - s->count < len && wait) {
        s->waiting++;
        bool more = wait_cv(&s->cv, &s->mu, wait, &until);
        s->waiting--;
        if (!more) {
            break;
        }
    }
    size_t n = s->size - s->count < len ? s->size - s->count : len;
    for (size_t i = 0; i < n; i++) {
        s->data[(s->head + s->count + i) % s->size] = ((const unsigned char *)data)[i];
    }
    s->count += n;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return n;
}

size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t wait)
{
    struct timespec until;
    deadline(&until, wait);
    pthread_mutex_lock(&s->mu);
    while (!s->count && wait) {
        s->waiting++;
        bool more = wait_cv(&s->cv, &s->mu, wait, &until);
        s->waiting--;
        if (!more) {
            break;
        }
    }
    size_t n = s->count < len ? s->count : len;
    for (size_t i = 0; i < n; i++) {
        ((unsigned char *)data)[i] = s->data[(s->head + i) % s->size];
    }
    s->head = (s->head + n) % s->size;
    s->count -= n;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return n;
}

size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s)
{
    pthread_mutex_lock(&s->mu);
    size_t n = s->count;
    pthread_mutex_unlock(&s->mu);
    return n;
}

BaseType_t xStreamBufferReset(StreamBufferHandle_t s)
{
    pthread_mutex_lock(&s->mu);
    if (s->waiting) {
        pthread_mutex_unlock(&s->mu);
        abort();   /* FreeRTOS refuses this; muse_tts.c must never do it */
    }
    s->head = s->count = 0;
    pthread_mutex_unlock(&s->mu);
    return pdPASS;
}

/* ---- Tasks ---- */

typedef struct {
    TaskFunction_t fn;
    void *arg;
} start_t;

static void *task_main(void *p)
{
    start_t st = *(start_t *)p;
    free(p);
    st.fn(st.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           unsigned prio, TaskHandle_t *out, int core, uint32_t caps)
{
    (void)name;
    (void)stack;
    (void)prio;
    (void)core;
    (void)caps;
    start_t *st = malloc(sizeof(*st));
    st->fn = fn;
    st->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, task_main, st)) {
        return pdFALSE;
    }
    pthread_detach(t);
    if (out) {
        *out = (TaskHandle_t)t;
    }
    return pdPASS;
}

/* ---- The scripted server ---- */

static pthread_mutex_t s_srv_mu = PTHREAD_MUTEX_INITIALIZER;
static fake_server_t s_server;
static char *s_last_body;
static char s_last_headers[16][2][128];
static int s_nheaders, s_opened, s_closed;

struct fake_http {
    esp_http_client_config_t cfg;
    fake_server_t srv;           /* the script as it was when this request was opened */
    char headers[16][2][128];
    int nheaders;
    int timeout_ms;
    int64_t headers_at, next_at;
    size_t off, body_len, chunk_left;
    bool headers_done;
};

void fake_server_set(const fake_server_t *s)
{
    pthread_mutex_lock(&s_srv_mu);
    s_server = *s;
    pthread_mutex_unlock(&s_srv_mu);
}

const char *fake_last_body(void)
{
    return s_last_body ? s_last_body : "";
}

const char *fake_last_header(const char *key)
{
    for (int i = 0; i < s_nheaders; i++) {
        if (!strcasecmp(s_last_headers[i][0], key)) {
            return s_last_headers[i][1];
        }
    }
    return "";
}

int fake_opened(void)
{
    return s_opened;
}

int fake_closed(void)
{
    return s_closed;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_ERR_HTTP_CONNECT ? "ESP_ERR_HTTP_CONNECT" : "ESP_FAIL";
}

static void sleep_until(int64_t t)
{
    int64_t now = esp_timer_get_time();
    if (t > now) {
        usleep((useconds_t)(t - now));
    }
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct fake_http *c = calloc(1, sizeof(*c));
    c->cfg = *config;
    c->timeout_ms = config->timeout_ms;
    return c;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    if (c->nheaders < 16) {
        strncpy(c->headers[c->nheaders][0], key, 127);
        strncpy(c->headers[c->nheaders][1], value, 127);
        c->nheaders++;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)write_len;
    pthread_mutex_lock(&s_srv_mu);
    c->srv = s_server;
    s_opened++;
    memcpy(s_last_headers, c->headers, sizeof(s_last_headers));
    s_nheaders = c->nheaders;
    pthread_mutex_unlock(&s_srv_mu);
    usleep((useconds_t)c->srv.connect_ms * 1000);
    if (c->srv.connect_fails) {
        return ESP_ERR_HTTP_CONNECT;
    }
    c->body_len = c->srv.body ? strlen(c->srv.body) : 0;
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len)
{
    pthread_mutex_lock(&s_srv_mu);
    free(s_last_body);
    s_last_body = strndup(buf, (size_t)len);
    pthread_mutex_unlock(&s_srv_mu);
    c->headers_at = esp_timer_get_time() + c->srv.headers_ms * 1000LL;
    return len;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    int64_t give_up = esp_timer_get_time() + c->timeout_ms * 1000LL;
    if (c->headers_at > give_up) {
        sleep_until(give_up);
        return -ESP_ERR_HTTP_EAGAIN;
    }
    sleep_until(c->headers_at);
    c->headers_done = true;
    if (c->srv.logid && c->cfg.event_handler) {
        esp_http_client_event_t e = {
            .event_id = HTTP_EVENT_ON_HEADER,
            .client = c,
            .user_data = c->cfg.user_data,
            .header_key = "X-Tt-Logid",
            .header_value = (char *)c->srv.logid,
        };
        c->cfg.event_handler(&e);
    }
    c->next_at = esp_timer_get_time() + c->srv.chunk_ms * 1000LL;
    c->chunk_left = c->srv.chunk ? c->srv.chunk : c->body_len;
    return 0;   /* chunked */
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c->headers_done ? c->srv.status : -1;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout_ms)
{
    c->timeout_ms = timeout_ms;
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    if (c->off >= c->body_len) {
        return 0;
    }
    int64_t give_up = esp_timer_get_time() + c->timeout_ms * 1000LL;
    bool stalled = c->srv.stall_at && c->off >= c->srv.stall_at;
    if (stalled || c->next_at > give_up) {
        sleep_until(give_up);
        return -ESP_ERR_HTTP_EAGAIN;
    }
    sleep_until(c->next_at);
    size_t n = (size_t)len < c->chunk_left ? (size_t)len : c->chunk_left;
    if (n > c->body_len - c->off) {
        n = c->body_len - c->off;
    }
    if (c->srv.stall_at && c->off < c->srv.stall_at && c->off + n > c->srv.stall_at) {
        n = c->srv.stall_at - c->off;
    }
    memcpy(buf, c->srv.body + c->off, n);
    c->off += n;
    c->chunk_left -= n;
    if (!c->chunk_left) {
        c->chunk_left = c->srv.chunk ? c->srv.chunk : c->body_len;
        c->next_at = esp_timer_get_time() + c->srv.chunk_ms * 1000LL;
    }
    return (int)n;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    return c->off >= c->body_len;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    (void)c;
    pthread_mutex_lock(&s_srv_mu);
    s_closed++;
    pthread_mutex_unlock(&s_srv_mu);
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    free(c);
    return ESP_OK;
}