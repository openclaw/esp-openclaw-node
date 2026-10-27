#include "esp_openclaw_directory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static esp_openclaw_directory_result_t parse(const char *json, size_t offset, esp_openclaw_agent_page_t *page)
{
    return esp_openclaw_directory_parse(json, strlen(json), offset, page);
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
    puts("Agent directory: identities, paging, UTF-8 bounds and invalid payloads passed");
    return 0;
}
