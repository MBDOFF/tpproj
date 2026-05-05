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
    if(curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;
        char url[512];
        snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", model_name);
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "X-goog-api-key: %s", api_key);
        headers = curl_slist_append(headers, auth_header);
        
        cJSON *root = cJSON_CreateObject();
        cJSON *contents = cJSON_AddArrayToObject(root, "contents");
        cJSON *content = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, content);
        cJSON *parts = cJSON_AddArrayToObject(content, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddItemToArray(parts, part);
        cJSON_AddStringToObject(part, "text", prompt);
        
        char *json_str = cJSON_PrintUnformatted(root);
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        
        int retries = 3;
        while (retries > 0) {
            res = curl_easy_perform(curl);
            if(res == CURLE_OK) {
                cJSON *resp_json = cJSON_Parse(chunk.memory);
                if (resp_json) {
                    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
                    if (error) {
                        char* err_str = cJSON_Print(error);
                        output = malloc(strlen(err_str) + 256);
                        sprintf(output, "Error processing API response. API Error:\n%s", err_str);
                        free(err_str);
                        cJSON_Delete(resp_json);
                        break;
                    }
                    
                    // Parse Output
                    cJSON *c = cJSON_GetObjectItem(resp_json, "candidates");
                    if (c && cJSON_GetArraySize(c) > 0) {
                        cJSON *cand = cJSON_GetArrayItem(c, 0);
                        cJSON *cont = cJSON_GetObjectItem(cand, "content");
                        cJSON *p = cJSON_GetObjectItem(cont, "parts");
                        if (p && cJSON_GetArraySize(p) > 0) {
                            cJSON *txt = cJSON_GetObjectItem(cJSON_GetArrayItem(p, 0), "text");
                            if (txt && cJSON_IsString(txt)) {
                                output = strdup(txt->valuestring);
                            }
                        }
                    }
                    
                    // Parse Usage Meta (Tokens)
                    if (out_tokens) {
                        cJSON *usage = cJSON_GetObjectItem(resp_json, "usageMetadata");
                        if (usage) {
                            cJSON *total = cJSON_GetObjectItem(usage, "totalTokenCount");
                            if (total) *out_tokens = total->valueint;
                        }
                    }
                    cJSON_Delete(resp_json);
                    break;
                }
            }
            retries--;
            sleep(1);
        }
        
        if (!output) {
            output = malloc(1024 + chunk.size);
            sprintf(output, "Error processing API response. Raw:\n%s", chunk.memory);
        }
        
        free(json_str);
        cJSON_Delete(root);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
    return output ? output : strdup("Error initializing CURL");
}
