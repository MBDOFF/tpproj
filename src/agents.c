#include "agents.h"
#include "llm.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

// Helper function to read all workspace files into memory context
static char* get_workspace_files(const char* run_id) {
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "playground/%s", run_id);
    DIR *d = opendir(dir_path);
    if (!d) return strdup("");

    char* out = calloc(1, 2 * 1024 * 1024); // 2MB buffer for large projects
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) {
            // Only cache source files and configs
            if (strstr(dir->d_name, ".c") || strstr(dir->d_name, ".h") || strstr(dir->d_name, "Makefile") || strstr(dir->d_name, ".sh")) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, dir->d_name);
                FILE* f = fopen(filepath, "r");
                if (f) {
                    strcat(out, "\n--- FILE: ");
                    strcat(out, dir->d_name);
                    strcat(out, " ---\n");
                    char buf[1024];
                    while (fgets(buf, sizeof(buf), f)) {
                        if (strlen(out) + strlen(buf) < (2 * 1024 * 1024) - 100) {
                            strcat(out, buf);
                        }
                    }
                    fclose(f);
                    strcat(out, "\n-------------------\n");
                }
            }
        }
    }
    closedir(d);
    return out;
}

char* run_agent_loop(const char* role_name, int color_id, const char* run_id, const char* system_prompt, const char* task) {
    char* context = calloc(1, 65536);
    snprintf(context, 65536, "Task: %s\n", task);
    char* final_result = NULL;
    
    while(1) {
        char* ws_cache = get_workspace_files(run_id);
        char* llm_prompt = malloc(strlen(context) + strlen(ws_cache) + 8192);
        
        sprintf(llm_prompt, "%s\n\n=== CURRENT WORKSPACE FILES (CACHE) ===\n%s\n======================================\n\nYou MUST use exactly one of these XML tags in your response:\n<bash>command</bash>\n<write file=\"path/filename.c\">content</write>\n<patch file=\"path/filename.c\">\n<old>\n...old code...\n</old>\n<new>\n...new code...\n</new>\n</patch>\n<message>final text</message>\n\nContext History:\n%s\n\n%s:", 
                system_prompt, ws_cache, context, role_name);
        
        free(ws_cache);
        
        char spinner_label[128];
        snprintf(spinner_label, sizeof(spinner_label), "[%s] Processing subroutine...", role_name);
        start_spinner(spinner_label);
        
        int tokens_used = 0;
        char* response = query_gemini(llm_prompt, &tokens_used);
        
        stop_spinner();
        ui_add_tokens(tokens_used);
        free(llm_prompt);
        
        if (strlen(context) + strlen(response) > 60000) {
            memmove(context, context + 30000, strlen(context) - 30000 + 1);
        }
        
        strcat(context, role_name);
        strcat(context, ": ");
        strcat(context, response);
        strcat(context, "\n");
        
        // Tracking to ensure at least one tag is parsed
        int tag_found = 0;
        
        // 1. Parse bash
        char* bash_start = strstr(response, "<bash>");
        if (bash_start) {
            tag_found = 1;
            bash_start += 6;
            char* bash_end = strstr(bash_start, "</bash>");
            if (bash_end) {
                *bash_end = '\0';
                ui_log(color_id, role_name, "Executing system command: %s", bash_start);
                
                char script_path[512];
                snprintf(script_path, sizeof(script_path), "playground/%s/run_cmd.sh", run_id);
                FILE* sf = fopen(script_path, "w");
                if (sf) {
                    fprintf(sf, "%s\n", bash_start);
                    fclose(sf);
                    
                    char cmd[2048];
                    snprintf(cmd, sizeof(cmd), "cd playground/%s && bash -c 'set -m; bash run_cmd.sh 2>&1 & PID=$!; (sleep 5; kill -9 -$PID 2>/dev/null) & wait $PID'", run_id);
                    FILE* fp = popen(cmd, "r");
                    char output[4096] = "";
                    if (fp) {
                        char line[256];
                        while(fgets(line, sizeof(line), fp)) {
                            if(strlen(output) + strlen(line) < 4000) strcat(output, line);
                        }
                        pclose(fp);
                    }
                    if (strlen(output) == 0) strcpy(output, "(success: no output)");
                    else if (strstr(output, "Killed")) strcpy(output, "(Terminated: Script timed out after 5 seconds due to infinite loop or blocking)");
                    
                    ui_log_raw(C_SYS, "-> System Output:\n%s", output);
                    
                    strcat(context, "System (Bash Output):\n");
                    strcat(context, output);
                    strcat(context, "\n");
                    
                    unlink(script_path);
                }
            }
        }
        
        // 2. Parse write
        char* write_start = strstr(response, "<write file=\"");
        if (write_start) {
            tag_found = 1;
            write_start += 13;
            char* name_end = strstr(write_start, "\">");
            if (name_end) {
                *name_end = '\0';
                char* content_start = name_end + 2;
                
                // Allow optional newline after ">"
                if (*content_start == '\n') content_start++;
                
                char* write_end = strstr(content_start, "</write>");
                if (write_end) {
                    *write_end = '\0';
                    char filepath[512];
                    snprintf(filepath, sizeof(filepath), "playground/%s/%s", run_id, write_start);
                    FILE* f = fopen(filepath, "w");
                    if (f) {
                        fputs(content_start, f);
                        fclose(f);
                        ui_add_file_mod();
                        ui_log(color_id, role_name, "Deployed file: %s", write_start);
                        strcat(context, "System: File written successfully.\n");
                    } else {
                        ui_log_raw(C_SYS, "-> System Error: Failed to write %s", write_start);
                        strcat(context, "System: Failed to write file.\n");
                    }
                }
            }
        }
        
        // 3. Parse patch
        char* patch_start = strstr(response, "<patch file=\"");
        if (patch_start) {
            tag_found = 1;
            char* f_start = patch_start + 13;
            char* f_end = strchr(f_start, '"');
            if (f_end) {
                *f_end = '\0';
                char filename[256];
                strncpy(filename, f_start, sizeof(filename)-1);
                
                char* old_start = strstr(f_end + 1, "<old>");
                char* new_start = strstr(f_end + 1, "<new>");
                if (old_start && new_start) {
                    old_start += 5;
                    if (*old_start == '\n') old_start++;
                    char* old_end = strstr(old_start, "</old>");
                    
                    new_start += 5;
                    if (*new_start == '\n') new_start++;
                    char* new_end = strstr(new_start, "</new>");
                    
                    if (old_end && new_end) {
                        if (*(old_end - 1) == '\n') old_end--;
                        if (*(new_end - 1) == '\n') new_end--;
                        
                        int old_len = old_end - old_start;
                        int new_len = new_end - new_start;
                        
                        char* old_str = malloc(old_len + 1);
                        strncpy(old_str, old_start, old_len);
                        old_str[old_len] = '\0';
                        
                        char* new_str = malloc(new_len + 1);
                        strncpy(new_str, new_start, new_len);
                        new_str[new_len] = '\0';
                        
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "playground/%s/%s", run_id, filename);
                        FILE* f = fopen(filepath, "r");
                        if (f) {
                            fseek(f, 0, SEEK_END);
                            long fsize = ftell(f);
                            fseek(f, 0, SEEK_SET);
                            char* content = calloc(1, fsize + 1);
                            fread(content, 1, fsize, f);
                            fclose(f);
                            
                            char* match = strstr(content, old_str);
                            if (match) {
                                FILE* out = fopen(filepath, "w");
                                fwrite(content, 1, match - content, out);
                                fputs(new_str, out);
                                fputs(match + old_len, out);
                                fclose(out);
                                ui_log(color_id, role_name, "Patched file: %s (replaced %d bytes)", filename, old_len);
                                strcat(context, "System: File patched successfully.\n");
                                ui_add_file_mod();
                            } else {
                                ui_log_raw(C_SYS, "-> System Error: Patch failed, <old> block not found in %s", filename);
                                strcat(context, "System: Patch failed, exact <old> string not found.\n");
                            }
                            free(content);
                        } else {
                            ui_log_raw(C_SYS, "-> System Error: File %s not found to patch", filename);
                            strcat(context, "System: File not found.\n");
                        }
                        
                        free(old_str);
                        free(new_str);
                    }
                }
            }
        }
        
        // 4. Parse message (WE BREAK THE LOOP HERE LAST)
        char* msg = strstr(response, "<message>");
        if (msg) {
            tag_found = 1;
            msg += 9;
            char* end = strstr(msg, "</message>");
            if (end) *end = '\0';
            ui_log(color_id, role_name, "%s", msg);
            final_result = strdup(msg);
            free(response);
            break; 
        }

        if (!tag_found) {
            ui_log_raw(C_SYS, "-> System Notice: Agent generated invalid XML. Forcing retry. Response was:\n%s", response);
            
            // If we got an API error, back off heavily
            if (strstr(response, "Error processing API response") || strstr(response, "Error initializing CURL")) {
                sleep(5);
            } else {
                strcat(context, "System: Invalid format. You MUST use exactly ONE of the requested XML tags (<bash>, <write>, <patch>, <message>). Try again.\n");
                sleep(1);
            }
        }
        
        free(response);
    }
    free(context);
    return final_result;
}
