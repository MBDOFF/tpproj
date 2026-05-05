#include <unistd.h>
#include "llm.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

char* query_gemini(const char* prompt, int* out_tokens) {
    CURL *curl;
    CURLcode res;
    char* output = NULL;
    if (out_tokens) *out_tokens = 0;
    
    char* api_key = getenv("GEMINI_API_KEY");
    char* model_name = getenv("GEMINI_MODEL");
    if (!api_key || !model_name) return strdup("Error: GEMINI_API_KEY or GEMINI_MODEL missing from .env");
    
    curl = curl_easy_init();
    if(!curl) return strdup("Error initializing CURL");
    
    char url[512];
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", model_name);
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "X-goog-api-key: %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    
    // Build JSON request
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(root, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON_AddItemToArray(contents, content);
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddItemToArray(parts, part);
    cJSON_AddStringToObject(part, "text", prompt);
    
    // Generation config: fast, deterministic
    cJSON *gen_config = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON_AddNumberToObject(gen_config, "temperature", 0.2);
    cJSON_AddNumberToObject(gen_config, "maxOutputTokens", 8192);
    cJSON_AddNumberToObject(gen_config, "topP", 0.8);
    
    char *json_str = cJSON_PrintUnformatted(root);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    
    int retries = 3;
    int backoff = 1;
    
    while (retries > 0 && !output) {
        // Fresh response buffer each attempt
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.memory[0] = '\0';
        chunk.size = 0;
        
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        
        res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            // CURL-level error (network, timeout, etc.)
            char err_msg[512];
            snprintf(err_msg, sizeof(err_msg), "CURL error: %s (attempt %d/3)", 
                curl_easy_strerror(res), 4 - retries);
            free(chunk.memory);
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup(err_msg);
            break;
        }
        
        // Check HTTP status code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        if (http_code == 429) {
            // Rate limited — back off heavily
            free(chunk.memory);
            retries--;
            if (retries > 0) { sleep(backoff * 3); backoff *= 2; continue; }
            output = strdup("Error: API rate limited (429). Try again in a moment.");
            break;
        }
        
        if (http_code != 200) {
            char err_msg[1024];
            snprintf(err_msg, sizeof(err_msg), "API error HTTP %ld: %.500s", http_code, 
                chunk.size > 0 ? chunk.memory : "(empty response)");
            free(chunk.memory);
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup(err_msg);
            break;
        }
        
        // HTTP 200 — parse JSON
        if (chunk.size == 0) {
            free(chunk.memory);
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup("Error: API returned empty response");
            break;
        }
        
        cJSON *resp_json = cJSON_Parse(chunk.memory);
        if (!resp_json) {
            char err_msg[512];
            snprintf(err_msg, sizeof(err_msg), "Error: Invalid JSON from API: %.200s", chunk.memory);
            free(chunk.memory);
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup(err_msg);
            break;
        }
        
        // Check for API error object
        cJSON *error = cJSON_GetObjectItem(resp_json, "error");
        if (error) {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            char err_msg[512];
            snprintf(err_msg, sizeof(err_msg), "API Error: %s", 
                (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            cJSON_Delete(resp_json);
            free(chunk.memory);
            
            // Some API errors are retryable (overloaded, etc.)
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup(err_msg);
            break;
        }
        
        // Parse candidates
        cJSON *c = cJSON_GetObjectItem(resp_json, "candidates");
        if (c && cJSON_GetArraySize(c) > 0) {
            cJSON *cand = cJSON_GetArrayItem(c, 0);
            cJSON *cont = cJSON_GetObjectItem(cand, "content");
            if (cont) {
                cJSON *p = cJSON_GetObjectItem(cont, "parts");
                if (p && cJSON_GetArraySize(p) > 0) {
                    cJSON *txt = cJSON_GetObjectItem(cJSON_GetArrayItem(p, 0), "text");
                    if (txt && cJSON_IsString(txt)) {
                        output = strdup(txt->valuestring);
                    }
                }
            }
            
            // Check if blocked by safety filter
            if (!output) {
                cJSON *finish = cJSON_GetObjectItem(cand, "finishReason");
                if (finish && cJSON_IsString(finish)) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "API blocked response: %s", finish->valuestring);
                    output = strdup(err_msg);
                }
            }
        }
        
        // Parse token usage
        if (out_tokens) {
            cJSON *usage = cJSON_GetObjectItem(resp_json, "usageMetadata");
            if (usage) {
                cJSON *total = cJSON_GetObjectItem(usage, "totalTokenCount");
                if (total) *out_tokens = total->valueint;
            }
        }
        
        cJSON_Delete(resp_json);
        free(chunk.memory);
        
        if (!output) {
            // Valid JSON but no text content
            retries--;
            if (retries > 0) { sleep(backoff); backoff *= 2; continue; }
            output = strdup("Error: API returned valid JSON but no text content");
        }
    }
    
    free(json_str);
    cJSON_Delete(root);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return output ? output : strdup("Error: all retries exhausted");
}
