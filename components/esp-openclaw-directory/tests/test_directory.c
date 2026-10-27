#include "esp_openclaw_directory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static esp_openclaw_directory_result_t parse(const char *json, size_t offset, esp_openclaw_agent_page_t *page)
{
    return esp_openclaw_directory_parse(json, strlen(json), offset, page);
}

static void conversation_cases(void)
{
    esp_openclaw_session_page_t page;
    const char *json = "{\"offset\":8,\"hasMore\":true,\"nextOffset\":9,\"sessions\":[{\"key\":\"agent:kitsune:work:daily\",\"agentId\":\"kitsune\",\"label\":\"Planning\",\"displayName\":\"Fallback\",\"lastMessagePreview\":\"Next steps 🦊\"}]}";
    assert(esp_openclaw_sessions_parse(json, strlen(json), "kitsune", 8, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(page.count == 1 && page.offset == 8 && page.has_more && page.next_offset == 9);
    assert(strcmp(page.sessions[0].key, "agent:kitsune:work:daily") == 0);
    assert(strcmp(page.sessions[0].title, "Planning") == 0);
    assert(strcmp(page.sessions[0].last_message, "Next steps 🦊") == 0);
    const char *invalid[] = {
        "{\"sessions\":[{\"key\":\"agent:other:main\"}]}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:main\",\"agentId\":\"other\"}]}",
        "{\"sessions\":[{\"key\":\"global\"}]}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:\"}]}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:main\\u0000suffix\"}]}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:main\"},{\"key\":\"agent:kitsune:main\"}]}",
        "{\"sessions\":[],\"offset\":1}",
        "{\"sessions\":[],\"offset\":0.5}",
        "{\"sessions\":[],\"hasMore\":true,\"nextOffset\":0}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:main\"}],\"hasMore\":true,\"nextOffset\":0}",
        "{\"sessions\":[{\"key\":\"agent:kitsune:main\"}],\"hasMore\":true,\"nextOffset\":2}",
        "{\"sessions\":[],\"hasMore\":\"false\"}",
        "{\"sessions\":[],\"nextOffset\":3}",
        "{\"sessions\":[]}trailing",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(esp_openclaw_sessions_parse(invalid[i], strlen(invalid[i]), "kitsune", 0, &page) == ESP_OPENCLAW_DIRECTORY_INVALID);
        assert(page.count == 0 && !page.has_more && page.sessions[0].key[0] == '\0');
    }
    const char *empty = "{\"sessions\":[],\"hasMore\":false,\"nextOffset\":null}";
    assert(esp_openclaw_sessions_parse(empty, strlen(empty), "kitsune", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK && page.count == 0);
    assert(esp_openclaw_sessions_parse(empty, strlen(empty), "Wrong ID", 0, &page) == ESP_OPENCLAW_DIRECTORY_INVALID);

    cJSON *root = cJSON_CreateObject();
    cJSON *rows = cJSON_AddArrayToObject(root, "sessions");
    cJSON *row = cJSON_CreateObject();
    cJSON_AddItemToArray(rows, row);
    char key[ESP_OPENCLAW_SESSION_KEY_BYTES + 1];
    memcpy(key, "agent:kitsune:", 14);
    memset(key + 14, 'x', sizeof(key) - 15);
    key[sizeof(key) - 1] = '\0';
    cJSON_AddStringToObject(row, "key", key);
    char *large = cJSON_PrintUnformatted(root);
    assert(esp_openclaw_sessions_parse(large, strlen(large), "kitsune", 0, &page) == ESP_OPENCLAW_DIRECTORY_TOO_LARGE && page.count == 0);
    cJSON_free(large);
    key[sizeof(key) - 2] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(row, "key", cJSON_CreateString(key));
    cJSON_AddStringToObject(row, "derivedTitle", "🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊");
    large = cJSON_PrintUnformatted(root);
    assert(esp_openclaw_sessions_parse(large, strlen(large), "kitsune", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(strcmp(page.sessions[0].key, key) == 0 && strlen(page.sessions[0].title) == 96);
    cJSON_free(large);
    cJSON_Delete(root);

    root = cJSON_CreateObject();
    rows = cJSON_AddArrayToObject(root, "sessions");
    for (unsigned i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE + 1; ++i) {
        row = cJSON_CreateObject();
        char id[64];
        snprintf(id, sizeof(id), "agent:kitsune:thread-%u", i);
        cJSON_AddStringToObject(row, "key", id);
        cJSON_AddItemToArray(rows, row);
    }
    large = cJSON_PrintUnformatted(root);
    assert(esp_openclaw_sessions_parse(large, strlen(large), "kitsune", 0, &page) == ESP_OPENCLAW_DIRECTORY_TOO_LARGE && page.count == 0);
    cJSON_free(large);
    cJSON_Delete(root);

    esp_openclaw_session_preview_t preview;
    const char *preview_json = "{\"previews\":[{\"key\":\"agent:kitsune:work:daily\",\"status\":\"ok\",\"items\":[{\"role\":\"user\",\"text\":\"Hello\"},{\"role\":\"assistant\",\"text\":\"Hi 🦊\"}]}]}";
    assert(esp_openclaw_preview_parse(preview_json, strlen(preview_json), "agent:kitsune:work:daily", &preview) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(preview.status == ESP_OPENCLAW_PREVIEW_OK && preview.count == 2);
    assert(preview.items[0].role == ESP_OPENCLAW_PREVIEW_USER && strcmp(preview.items[1].text, "Hi 🦊") == 0);
    assert(esp_openclaw_preview_parse(preview_json, strlen(preview_json), "agent:kitsune:other", &preview) == ESP_OPENCLAW_DIRECTORY_INVALID && preview.count == 0);
    const char *states[] = {"empty", "missing", "error"};
    for (size_t i = 0; i < 3; ++i) {
        char payload[160];
        snprintf(payload, sizeof(payload), "{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"%s\",\"items\":[]}]}", states[i]);
        assert(esp_openclaw_preview_parse(payload, strlen(payload), "agent:kitsune:main", &preview) == ESP_OPENCLAW_DIRECTORY_OK);
        assert(preview.count == 0 && preview.status == (esp_openclaw_preview_status_t)(i + 1));
    }
    const char *bad_previews[] = {
        "{\"previews\":[]}",
        "{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"ok\",\"items\":[]}]}",
        "{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"missing\",\"items\":[{\"role\":\"user\",\"text\":\"hidden\"}]}]}",
        "{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"ok\",\"items\":[{\"role\":\"unknown\",\"text\":\"text\"}]}]}",
        "{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"ok\",\"items\":[{\"role\":\"user\",\"text\":42}]}]}",
    };
    for (size_t i = 0; i < sizeof(bad_previews) / sizeof(bad_previews[0]); ++i) {
        assert(esp_openclaw_preview_parse(bad_previews[i], strlen(bad_previews[i]), "agent:kitsune:main", &preview) == ESP_OPENCLAW_DIRECTORY_INVALID);
        assert(preview.count == 0 && preview.status == ESP_OPENCLAW_PREVIEW_ERROR);
    }
    root = cJSON_CreateObject();
    rows = cJSON_AddArrayToObject(root, "previews");
    row = cJSON_CreateObject();
    cJSON_AddItemToArray(rows, row);
    cJSON_AddStringToObject(row, "key", "agent:kitsune:main");
    cJSON_AddStringToObject(row, "status", "ok");
    cJSON *items = cJSON_AddArrayToObject(row, "items");
    char message[260];
    for (unsigned i = 0; i < 64; ++i) memcpy(message + i * 4, "🦊", 4);
    message[256] = '\0';
    for (unsigned i = 0; i < ESP_OPENCLAW_PREVIEW_ITEMS + 1; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", "assistant");
        cJSON_AddStringToObject(item, "text", message);
        cJSON_AddItemToArray(items, item);
    }
    large = cJSON_PrintUnformatted(root);
    assert(esp_openclaw_preview_parse(large, strlen(large), "agent:kitsune:main", &preview) == ESP_OPENCLAW_DIRECTORY_TOO_LARGE && preview.count == 0);
    cJSON_free(large);
    cJSON_DeleteItemFromArray(items, 4);
    large = cJSON_PrintUnformatted(root);
    assert(esp_openclaw_preview_parse(large, strlen(large), "agent:kitsune:main", &preview) == ESP_OPENCLAW_DIRECTORY_OK && preview.count == 4);
    assert(strlen(preview.items[0].text) == 240 && (unsigned char)preview.items[0].text[239] == 0x8a);
    cJSON_free(large);
    cJSON_Delete(root);
}

int main(void)
{
    esp_openclaw_agent_page_t page;
    const char *identity = "{\"agents\":[{\"id\":\"kitsune\",\"name\":\"Work\",\"identity\":{\"name\":\"Kitsune\",\"emoji\":\"🦊\",\"avatarUrl\":\"https://example.com/avatar\"}},{\"id\":\"builder\",\"name\":\"Builder\"},{\"id\":\"quiet\"}]}";
    assert(parse(identity, 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(page.count == 3 && page.total == 3 && page.offset == 0);
    assert(strcmp(page.agents[0].id, "kitsune") == 0 && strcmp(page.agents[0].name, "Kitsune") == 0);
    assert(strcmp(page.agents[0].emoji, "🦊") == 0);
    assert(strcmp(page.agents[1].name, "Builder") == 0 && strcmp(page.agents[2].name, "quiet") == 0);
    assert(page.main_key[0] == '\0');

    assert(parse("{\"mainKey\":\"work:daily\",\"agents\":[{\"id\":\"kitsune\"}]}", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(strcmp(page.main_key, "work:daily") == 0);
    assert(parse("{\"mainKey\":\"会話\",\"agents\":[]}", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(strcmp(page.main_key, "会話") == 0);
    assert(parse("{\"mainKey\":42,\"agents\":[]}", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(page.main_key[0] == '\0');

    char main_key[130];
    memset(main_key, 'x', sizeof(main_key) - 1);
    main_key[sizeof(main_key) - 1] = '\0';
    cJSON *routing = cJSON_CreateObject();
    cJSON_AddArrayToObject(routing, "agents");
    cJSON_AddStringToObject(routing, "mainKey", main_key);
    char *routing_json = cJSON_PrintUnformatted(routing);
    assert(parse(routing_json, 0, &page) == ESP_OPENCLAW_DIRECTORY_OK && page.main_key[0] == '\0');
    cJSON_free(routing_json);
    main_key[128] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(routing, "mainKey", cJSON_CreateString(main_key));
    routing_json = cJSON_PrintUnformatted(routing);
    assert(parse(routing_json, 0, &page) == ESP_OPENCLAW_DIRECTORY_OK && strcmp(page.main_key, main_key) == 0);
    cJSON_free(routing_json);
    cJSON_Delete(routing);

    cJSON *root = cJSON_CreateObject();
    cJSON *rows = cJSON_AddArrayToObject(root, "agents");
    for (unsigned i = 0; i < 10; ++i) {
        cJSON *row = cJSON_CreateObject();
        char id[20];
        snprintf(id, sizeof(id), "agent-%u", i);
        cJSON_AddStringToObject(row, "id", id);
        cJSON_AddStringToObject(row, "name", "A long identity 🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊🦊");
        cJSON_AddItemToArray(rows, row);
    }
    char *json = cJSON_PrintUnformatted(root);
    assert(parse(json, 0, &page) == ESP_OPENCLAW_DIRECTORY_OK && page.count == 8 && page.total == 10);
    assert(strcmp(page.agents[7].id, "agent-7") == 0);
    assert(strlen(page.agents[0].name) < 65 && (unsigned char)page.agents[0].name[strlen(page.agents[0].name) - 1] == 0x8a);
    assert(parse(json, 8, &page) == ESP_OPENCLAW_DIRECTORY_OK && page.count == 2 && page.offset == 8);
    assert(strcmp(page.agents[0].id, "agent-8") == 0);
    assert(parse(json, 11, &page) == ESP_OPENCLAW_DIRECTORY_INVALID && page.count == 0);
    cJSON_free(json);
    cJSON_Delete(root);

    assert(parse("{\"agents\":[]}", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK && page.total == 0);
    const char *invalid[] = {
        "{}", "{\"agents\":{}}", "{\"agents\":[{}]}",
        "{\"agents\":[{\"id\":\"a\"},{\"id\":\"a\"}]}",
        "{\"agents\":[{\"id\":\"Wrong ID\"}]}",
        "{\"mainKey\":\"work\",\"agents\":[{\"id\":\"Wrong ID\"}]}",
        "{\"agents\":[{\"id\":\"01234567890123456789012345678901234567890123456789012345678901234\"}]}",
        "{\"agents\":[]}garbage",
        "{\"agents\":[{\"id\":\"main\\u0000suffix\"}]}",
        "{\"agents\":[{\"id\":\"main\\uZZZZsuffix\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"raw\nnewline\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\x80\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\xc0\xaf\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\xe0\x80\xaf\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\xed\xa0\x80\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\xf4\x90\x80\x80\"}]}",
        "{\"agents\":[{\"id\":\"main\",\"name\":\"bad\xe2\x82\"}]}",
        "{\"agents\":[],\"deep\":[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(parse(invalid[i], 0, &page) == ESP_OPENCLAW_DIRECTORY_INVALID && page.count == 0 && page.main_key[0] == '\0');
    }
    const char embedded_nul[] = "{\"agents\":[]}\0hidden";
    assert(esp_openclaw_directory_parse(embedded_nul, sizeof(embedded_nul) - 1, 0, &page) == ESP_OPENCLAW_DIRECTORY_INVALID);
    assert(parse("{\"agents\":[{\"id\":\"main\",\"name\":\"literal \\\\u0000\"}]}", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    assert(esp_openclaw_directory_parse("", ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1, 0, &page) == ESP_OPENCLAW_DIRECTORY_TOO_LARGE);
    assert(parse(" {\"agents\":[{\"id\":\"quoted\",\"name\":\"a \\\" { [\"}]} \n", 0, &page) == ESP_OPENCLAW_DIRECTORY_OK);
    conversation_cases();
    puts("Directory: agent identities, conversation routing, preview states and UTF-8 bounds passed");
    return 0;
}
