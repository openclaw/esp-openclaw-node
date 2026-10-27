#include "esp_openclaw_avatar.h"

#include <string.h>

extern const uint8_t avatar_pixels[] __asm__("_binary_agent_avatars_argb8888_start");

static bool emoji_matches(const char *value, const char *emoji)
{
    size_t length = strlen(emoji);
    return strncmp(value, emoji, length) == 0 &&
        (value[length] == '\0' || strcmp(value + length, "\xef\xb8\x8f") == 0);
}

static bool ascii_letter(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static char initial(unsigned char c)
{
    return (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
}

esp_openclaw_avatar_t esp_openclaw_agent_avatar(const esp_openclaw_agent_t *agent)
{
    esp_openclaw_avatar_t avatar = {0};
    if (emoji_matches(agent->emoji, "🦊")) avatar.kind = ESP_OPENCLAW_AVATAR_FOX;
    else if (emoji_matches(agent->emoji, "🦞")) avatar.kind = ESP_OPENCLAW_AVATAR_LOBSTER;
    else if (agent->emoji[0] >= 33 && agent->emoji[0] <= 126 && agent->emoji[1] == '\0')
        avatar.text[0] = agent->emoji[0];
    else {
        unsigned count = 0;
        bool word = true;
        for (const unsigned char *p = (const unsigned char *)agent->name; *p && count < 2; ++p) {
            if (word && ascii_letter(*p)) avatar.text[count++] = initial(*p);
            word = *p == ' ' || *p == '-' || *p == '_';
        }
        if (count == 0) {
            for (const unsigned char *p = (const unsigned char *)agent->id; *p; ++p) {
                if (ascii_letter(*p)) { avatar.text[0] = initial(*p); break; }
            }
            if (avatar.text[0] == '\0') avatar.text[0] = '?';
        }
    }
    return avatar;
}

const uint8_t *esp_openclaw_avatar_pixels(esp_openclaw_avatar_kind_t kind)
{
    if (kind != ESP_OPENCLAW_AVATAR_FOX && kind != ESP_OPENCLAW_AVATAR_LOBSTER) return NULL;
    return avatar_pixels + (kind - ESP_OPENCLAW_AVATAR_FOX) * ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4;
}
