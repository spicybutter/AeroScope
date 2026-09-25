#include "captive_dns.h"

#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace captive_dns {

static const char* TAG = "dns";

static uint32_t HotspotIp()
{
    esp_netif_ip_info_t info = {};
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) esp_netif_get_ip_info(ap, &info);
    return info.ip.addr;  // network byte order
}

static void DnsTask(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot bind UDP 53");
        if (sock >= 0) close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    for (;;) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (len < 12) continue;

        // Find the end of the first question (QNAME labels + QTYPE + QCLASS).
        int pos = 12;
        while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 1 + 4;
        if (pos > len) continue;
        const uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];

        buf[2] = 0x81;  // QR=1, opcode 0, RD copied as set
        buf[3] = 0x80;  // RA=1, RCODE 0
        buf[4] = 0; buf[5] = 1;                    // QDCOUNT 1
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; // ANCOUNT: answer A queries only
        memset(&buf[8], 0, 4);                     // NSCOUNT, ARCOUNT
        int out = pos;
        if (qtype == 1) {
            const uint32_t ip = HotspotIp();
            const uint8_t answer[] = {
                0xC0, 0x0C,              // name: pointer to the question
                0x00, 0x01, 0x00, 0x01,  // type A, class IN
                0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
                0x00, 0x04,              // RDLENGTH
            };
            memcpy(&buf[out], answer, sizeof(answer));
            out += sizeof(answer);
            memcpy(&buf[out], &ip, 4);
            out += 4;
        }
        sendto(sock, buf, out, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
}

void Start()
{
    xTaskCreate(DnsTask, "captive_dns", 4096, nullptr, 4, nullptr);
}

}  // namespace captive_dns
