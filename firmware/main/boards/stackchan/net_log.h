// Mochi v19 network log (Tai 24-Oct-2026: 'monitor wireless if I plug her into a power cube'). Every ESP log line still
// goes to USB as before, and is also sent as UDP datagrams to the gateway PC (the host in NVS websocket.url), port 8771,
// where the background robot log (scripts/robot_log_bg.py) writes them like the serial lines. Never blocks the logger:
// lines go through a ring buffer and a low-priority task sends them; nothing is queued from an interrupt.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include "settings.h"

namespace netlog {

constexpr int kPort = 8771;
inline vprintf_like_t g_orig = nullptr;
inline RingbufHandle_t g_rb = nullptr;

inline int Hook(const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    const int r = g_orig ? g_orig(fmt, ap) : vprintf(fmt, ap);
    if (g_rb != nullptr && !xPortInIsrContext()) {
        char buf[240];
        int n = vsnprintf(buf, sizeof(buf), fmt, copy);
        if (n > 0) xRingbufferSend(g_rb, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1, 0);
    }
    va_end(copy);
    return r;
}

inline bool HostFromNvs(std::string& host) {   // "ws://192.168.0.236:8765/..." -> "192.168.0.236"
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    const size_t a = url.find("://");
    if (a == std::string::npos) return false;
    const size_t b = url.find_first_of(":/", a + 3);
    host = url.substr(a + 3, b == std::string::npos ? std::string::npos : b - (a + 3));
    return !host.empty();
}

inline void Task(void*) {
    int sock = -1;
    sockaddr_in dst = {};
    for (;;) {
        size_t len = 0;
        char* item = static_cast<char*>(xRingbufferReceive(g_rb, &len, pdMS_TO_TICKS(2000)));
        if (sock < 0) {   // after Wi-Fi is up: one UDP socket to the gateway host
            esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip = {};
            std::string host;
            if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0 && HostFromNvs(host)) {
                dst.sin_family = AF_INET;
                dst.sin_port = htons(kPort);
                if (inet_pton(AF_INET, host.c_str(), &dst.sin_addr) == 1) sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            }
        }
        if (item != nullptr) {
            if (sock >= 0) sendto(sock, item, len, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            vRingbufferReturnItem(g_rb, item);
        }
    }
}

inline void Start() {
    if (g_rb != nullptr) return;
    g_rb = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_NOSPLIT);   // ~16 KB of pending lines; a full buffer drops lines
    if (g_rb == nullptr) return;
    xTaskCreate(Task, "net_log", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    g_orig = esp_log_set_vprintf(Hook);
}

}  // namespace netlog
