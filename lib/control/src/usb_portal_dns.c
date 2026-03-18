#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "portal_dns";

#define DNS_PORT 53
#define DNS_BUF_SIZE 512

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static int dns_sock = -1;
static TaskHandle_t dns_task_handle = NULL;

static int build_dns_response(const uint8_t *query, int query_len, uint8_t *resp, int resp_size) {
    if (query_len < (int)sizeof(dns_header_t) + 5) return -1;
    
    const dns_header_t *qhdr = (const dns_header_t *)query;
    
    uint16_t flags = ntohs(qhdr->flags);
    if ((flags & 0x8000) != 0) return -1;
    
    if (query_len > resp_size - 16) return -1;
    memcpy(resp, query, query_len);
    
    dns_header_t *rhdr = (dns_header_t *)resp;
    rhdr->flags = htons(0x8180);
    rhdr->ancount = qhdr->qdcount;
    rhdr->nscount = 0;
    rhdr->arcount = 0;
    
    int off = query_len;
    
    uint16_t qdcount = ntohs(qhdr->qdcount);
    const uint8_t *qptr = query + sizeof(dns_header_t);
    
    for (int q = 0; q < qdcount && q < 2; q++) {
        const uint8_t *name_start = qptr;
        while (*qptr != 0 && qptr < query + query_len) {
            qptr += *qptr + 1;
        }
        qptr++;
        qptr += 4;
        
        if (off + 16 > resp_size) break;
        
        uint16_t name_offset = htons(0xC000 | (uint16_t)(name_start - query));
        memcpy(resp + off, &name_offset, 2); off += 2;
        
        uint16_t type = htons(1);
        memcpy(resp + off, &type, 2); off += 2;
        
        uint16_t cls = htons(1);
        memcpy(resp + off, &cls, 2); off += 2;
        
        uint32_t ttl = htonl(60);
        memcpy(resp + off, &ttl, 4); off += 4;
        
        uint16_t rdlen = htons(4);
        memcpy(resp + off, &rdlen, 2); off += 2;
        
        uint8_t ip[4] = {10, 48, 0, 1};
        memcpy(resp + off, ip, 4); off += 4;
    }
    
    return off;
}

static void dns_server_task(void *arg) {
    uint8_t rx_buf[DNS_BUF_SIZE];
    uint8_t tx_buf[DNS_BUF_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    while (1) {
        int len = recvfrom(dns_sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        int resp_len = build_dns_response(rx_buf, len, tx_buf, sizeof(tx_buf));
        if (resp_len > 0) {
            sendto(dns_sock, tx_buf, resp_len, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }
}

esp_err_t portal_dns_start(void) {
    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        return ESP_FAIL;
    }
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    if (bind(dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(dns_sock);
        dns_sock = -1;
        return ESP_FAIL;
    }
    
    xTaskCreatePinnedToCore(dns_server_task, "dns_srv", 3072, NULL, 2, &dns_task_handle, 0);
    
    ESP_LOGI(TAG, "DNS catch-all server started on port %d", DNS_PORT);
    return ESP_OK;
}
