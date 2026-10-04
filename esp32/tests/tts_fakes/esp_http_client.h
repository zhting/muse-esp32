/* SPDX-License-Identifier: Apache-2.0 */
/* The parts of esp_http_client that components/muse/muse_tts.c uses, answered
 * by a scripted server (fake_http.h) for tests/muse_tts_harness.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_BASE 0x7000
#define ESP_ERR_HTTP_CONNECT (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)

typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef enum {
    HTTP_EVENT_ERROR,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
} esp_http_client_event_id_t;

typedef struct fake_http *esp_http_client_handle_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *conf);
    http_event_handle_cb event_handler;
    void *user_data;
    bool disable_auto_redirect;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout_ms);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
const char *esp_err_to_name(esp_err_t err);