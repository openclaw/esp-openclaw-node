#include "openclaw_connection_details.h"

#include <stdio.h>
#include <string.h>

void openclaw_wifi_details_format(char *text, size_t size,
                                 const esp_openclaw_node_wifi_status_t *wifi)
{
    char ssid[sizeof(wifi->ssid)] = {0};
    size_t used = 0;
    /* SSIDs are bytes, not necessarily text. Keep malformed bytes and line
     * breaks from becoming layout or status text; preserve readable UTF-8. */
    size_t length = strnlen(wifi->ssid, sizeof(wifi->ssid) - 1);
    for (size_t i = 0; i < length;) {
        unsigned char first = (unsigned char)wifi->ssid[i];
        size_t width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = width != 0 && width <= length - i;
        for (size_t j = 1; valid && j < width; ++j)
            valid = ((unsigned char)wifi->ssid[i + j] & 0xc0) == 0x80;
        if (valid && width > 1) {
            unsigned char second = (unsigned char)wifi->ssid[i + 1];
            valid = !((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90) ||
                (first == 0xc2 && second < 0xa0));
        }
        if (!valid || first < 32 || first == 127) {
            ssid[used++] = '?';
            ++i;
        } else {
            memcpy(ssid + used, wifi->ssid + i, width);
            used += width;
            i += width;
        }
    }
    if (!wifi->has_saved_network) {
        snprintf(text, size, "Wi-Fi not set up\n\nConnect by USB to save a network.\n\nPhone pairing is not available yet.");
    } else if (!wifi->connected) {
        char reason[40] = "";
        if (wifi->last_disconnect_reason != 0)
            snprintf(reason, sizeof(reason), "\n\nDisconnect code: %u", wifi->last_disconnect_reason);
        snprintf(text, size, "Wi-Fi offline\n\nSaved network\n%s%s\n\nCheck the router and signal. If settings changed, update Wi-Fi over USB.", ssid, reason);
    } else {
        char signal[48] = "Unavailable";
        if (wifi->ap_info_valid)
            snprintf(signal, sizeof(signal), "%d dBm / ch %u", wifi->rssi, wifi->channel);
        snprintf(text, size, "Wi-Fi connected\n\nNetwork\n%s\n\nSignal\n%s\n\nIP address\n%s",
            ssid, signal, wifi->ip[0] != '\0' ? wifi->ip : "Unavailable");
    }
}
