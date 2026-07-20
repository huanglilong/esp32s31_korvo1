/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "captive_dns.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";

#define DNS_PORT          53
#define DNS_BUF_SIZE      512
#define DHCPS_OFFER_DNS   0x02

static _Atomic TaskHandle_t s_dns_task;
static atomic_bool s_running;
static captive_dns_config_t s_config;
/* PSRAM-allocated stack for captive DNS task (pure network, no flash ops) */
#define DNS_TASK_STACK_SIZE 3072
static StackType_t *s_dns_stack = NULL;
static StaticTask_t s_dns_tcb;

static esp_err_t captive_dns_get_redirect_ip(uint32_t *redirect_ip)
{
    if (!redirect_ip) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_config.redirect_ip != 0) {
        *redirect_ip = s_config.redirect_ip;
        return ESP_OK;
    }

    if (!s_config.ap_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err = esp_netif_get_ip_info(s_config.ap_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *redirect_ip = ntohl(ip_info.ip.addr);
    return ESP_OK;
}

static esp_err_t captive_dns_configure_dhcp_dns(void)
{
    if (!s_config.ap_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_config.ap_netif, &ip_info);
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;

    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcps_get_status(s_config.ap_netif, &status);
    const bool was_started = (status == ESP_NETIF_DHCP_STARTED);
    if (was_started) {
        esp_netif_dhcps_stop(s_config.ap_netif);
    }

    uint8_t offer_dns = DHCPS_OFFER_DNS;
    esp_err_t err = esp_netif_dhcps_option(s_config.ap_netif,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns,
                                           sizeof(offer_dns));
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(s_config.ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    if (was_started) {
        esp_netif_dhcps_start(s_config.ap_netif);
    }

    return err;
}

static int dns_build_response(const uint8_t *req, int req_len,
                              uint8_t *resp, int resp_max,
                              uint32_t redirect_ip)
{
    if (req_len < 12 || resp_max < req_len + 16) {
        return -1;
    }

    memcpy(resp, req, req_len);
    resp[2] = 0x81;
    resp[3] = 0x80;
    resp[6] = 0x00;
    resp[7] = 0x01;

    int pos = req_len;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x00;
    resp[pos++] = 0x00; resp[pos++] = 0x3C;
    resp[pos++] = 0x00; resp[pos++] = 0x04;
    resp[pos++] = (redirect_ip >> 24) & 0xFF;
    resp[pos++] = (redirect_ip >> 16) & 0xFF;
    resp[pos++] = (redirect_ip >>  8) & 0xFF;
    resp[pos++] = (redirect_ip      ) & 0xFF;
    return pos;
}

static void dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
        if (s_dns_stack) { heap_caps_free(s_dns_stack); s_dns_stack = NULL; }
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        atomic_store_explicit(&s_running, false, memory_order_release);
        atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
        if (s_dns_stack) { heap_caps_free(s_dns_stack); s_dns_stack = NULL; }
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t redirect_ip_host = 0;
    if (captive_dns_get_redirect_ip(&redirect_ip_host) != ESP_OK) {
        close(sock);
        atomic_store_explicit(&s_running, false, memory_order_release);
        atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
        if (s_dns_stack) { heap_caps_free(s_dns_stack); s_dns_stack = NULL; }
        vTaskDelete(NULL);
        return;
    }

    static EXT_RAM_BSS_ATTR uint8_t buf[DNS_BUF_SIZE];
    static EXT_RAM_BSS_ATTR uint8_t resp[DNS_BUF_SIZE + 16];

    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (len < 0) {
            continue;
        }
        int resp_len = dns_build_response(buf, len, resp, sizeof(resp), redirect_ip_host);
        if (resp_len > 0) {
            sendto(sock, resp, resp_len, 0, (struct sockaddr *)&src, src_len);
        }
    }

    close(sock);
    ESP_LOGI(TAG, "Captive DNS stopped");
    atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
    if (s_dns_stack) { heap_caps_free(s_dns_stack); s_dns_stack = NULL; }
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(const captive_dns_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If a DNS task handle exists but the task is no longer running
     * (stopped by captive_dns_stop()), wait for it to exit and clear
     * the handle. Without this, a rapid stop-then-start sequence (e.g.
     * STA connect → DNS stop → STA disconnect → DNS start within the
     * 1-second recvfrom timeout) would see s_dns_task still non-null
     * and silently return, leaving the captive portal permanently
     * broken. */
    if (atomic_load_explicit(&s_dns_task, memory_order_acquire) &&
        !atomic_load_explicit(&s_running, memory_order_acquire)) {
        /* Poll with short back-off until the old task clears its handle.
         * The DNS task uses recvfrom with 1-second timeout, so worst-case
         * wait is ~1s. Using 10ms ticks keeps the event loop responsive. */
        int retries = 0;
        const int max_retries = 120; /* 120 * 10ms = 1.2s */
        while (atomic_load_explicit(&s_dns_task, memory_order_acquire) && retries < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(10));
            retries++;
        }
        if (atomic_load_explicit(&s_dns_task, memory_order_acquire)) {
            ESP_LOGW(TAG, "Timed out waiting for old DNS task to exit");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (atomic_load_explicit(&s_dns_task, memory_order_acquire)) {
        return ESP_OK;
    }

    s_config = *config;

    if (s_config.configure_dhcp_dns) {
        esp_err_t err = captive_dns_configure_dhcp_dns();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure DHCP DNS: %s", esp_err_to_name(err));
        }
    }

    uint32_t redirect_ip = 0;
    esp_err_t err = captive_dns_get_redirect_ip(&redirect_ip);
    if (err != ESP_OK) {
        return err;
    }

    atomic_store_explicit(&s_running, true, memory_order_release);
    /* Allocate PSRAM stack for DNS task (pure network, no flash operations) */
    s_dns_stack = (StackType_t *)heap_caps_malloc(
        DNS_TASK_STACK_SIZE * sizeof(StackType_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dns_stack) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
        return ESP_FAIL;
    }
    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        dns_task, "captive_dns", DNS_TASK_STACK_SIZE,
        NULL, 5, s_dns_stack, &s_dns_tcb, tskNO_AFFINITY);
    if (task_handle == NULL) {
        heap_caps_free(s_dns_stack);
        s_dns_stack = NULL;
        atomic_store_explicit(&s_running, false, memory_order_release);
        atomic_store_explicit(&s_dns_task, NULL, memory_order_release);
        return ESP_FAIL;
    }
    atomic_store_explicit(&s_dns_task, task_handle, memory_order_release);
    return ESP_OK;
}

void captive_dns_stop(void)
{
    atomic_store_explicit(&s_running, false, memory_order_release);
}
