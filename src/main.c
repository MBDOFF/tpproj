#include "agents.h"
#include "cJSON.h"
#include "env.h"
#include "ui.h"
#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_ITERATIONS 5

void run_onboarding(char* prompt_buf, char* run_id) {
    if (strlen(prompt_buf) == 0) {
        ui_get_input("What are we cooking today? ", prompt_buf, 4095);
    }

    start_spinner("Querying options...");
    char q_prompt[4096];
    snprintf(q_prompt, sizeof(q_prompt), "The user wants to build a C project: '%s'. Provide exactly 3 distinct architectural options to narrow down the scope. Return ONLY a valid JSON array of 3 strings. NO MARKDOWN. Example: [\"Option 1\", \"Option 2\", \"Option 3\"]", prompt_buf);
    
    int tokens = 0;
    char* options_json = query_gemini(q_prompt, &tokens);
    stop_spinner();
    ui_add_tokens(tokens);

    char* parsed_options[4];
    char buf1[256] = "Terminal ncurses grid based";
    char buf2[256] = "SDL2/Raylib 2D GUI";
    char buf3[256] = "Networked multiplayer server";
    char buf4[256] = "> Custom Input (Type your own)";
    
    parsed_options[0] = buf1;
    parsed_options[1] = buf2;
    parsed_options[2] = buf3;
    parsed_options[3] = buf4;

    char* json_start = strchr(options_json, '[');
    char* json_end = strrchr(options_json, ']');
    if (json_start && json_end && json_end > json_start) {
        *(json_end + 1) = '\0';
        cJSON* arr = cJSON_Parse(json_start);
        if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 3) {
            for (int i = 0; i < 3; i++) {
                cJSON* item = cJSON_GetArrayItem(arr, i);
                if (item && cJSON_IsString(item)) {
                    strncpy(parsed_options[i], item->valuestring, 255);
                    parsed_options[i][255] = '\0'; 
                }
            }
        } else {
            ui_log_raw(C_SYS, "-> System Notice: JSON Array Parse failed. Falling back to defaults.");
        }
        if (arr) cJSON_Delete(arr);
    } else {
        ui_log_raw(C_SYS, "-> System Notice: Failed to find JSON array. Falling back to defaults.");
    }
    
    int choice = ui_select_menu(" Select project scope (Use Arrow Keys + Enter): ", parsed_options, 4);
    
    char answers[1024];
    if (choice == 3) {
        ui_get_input("Type your custom vision: ", answers, 1023);
    } else {
        strncpy(answers, parsed_options[choice], 1023);
    }
    free(options_json);

    start_spinner("Scaffolding playground...");
    char p_prompt[8192];
    snprintf(p_prompt, sizeof(p_prompt), "User wants: '%s'. Scope chosen: '%s'. Write a bash script that sets up the basic file structure and boilerplate C files for this project (e.g. main.c, Makefile) in the CURRENT directory. Return ONLY the bash script inside ```bash and ``` tags.", prompt_buf, answers);
    char* bash_script = query_gemini(p_prompt, &tokens);
    stop_spinner();
    ui_add_tokens(tokens);

    char* start = strstr(bash_script, "```bash");
    if (start) {
        start += 7;
        char* end = strstr(start, "```");
        if (end) *end = '\0';
        char script_path[256];
        snprintf(script_path, sizeof(script_path), "playground/%s/setup.sh", run_id);
        FILE* script_file = fopen(script_path, "w");
        if (script_file) {
            fprintf(script_file, "%s", start);
            fclose(script_file);
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "cd playground/%s && bash setup.sh > /dev/null 2>&1 && rm setup.sh", run_id);
            system(cmd);
        }
    }
    free(bash_script);

    char final_prompt[4096];
    snprintf(final_prompt, sizeof(final_prompt), "Project: %s. User choice: %s.", prompt_buf, answers);
    strncpy(prompt_buf, final_prompt, 4095);
    
    ui_log(C_KING, "Kernel", "Environment deployed in ./playground/%s", run_id);
}

int main(int argc, char** argv) {
    srand(time(NULL) ^ getpid());
    load_env();
    
    char run_id[32];
    sprintf(run_id, "%08X", (unsigned int)(time(NULL) ^ rand()));
    
    mkdir("playground", 0777);
    char path[256];
    snprintf(path, sizeof(path), "playground/%s", run_id);
    mkdir(path, 0777);
    
    char prompt_buf[4096] = {0};
    if (argc > 1) {
        strncpy(prompt_buf, argv[1], sizeof(prompt_buf) - 1);
    }
    
    // START GUI
    ui_init(run_id);
    
    run_onboarding(prompt_buf, run_id);
    
    ui_log_raw(C_KING, "\n=============================================");
    ui_log_raw(C_KING, " COURT IN SESSION ");
    ui_log_raw(C_KING, "=============================================\n");
    
    ui_log(C_KING, "Kernel", "Initiating primary build pipeline.");
    int consensus = 0;
    
    char* dev_task = malloc(8192);
    strcpy(dev_task, prompt_buf);
    
    int iteration = 0;
    
    while(!consensus && iteration < MAX_ITERATIONS) {
        ui_log(C_KING, "Kernel", "Delegating to Dev (iteration %d/%d)", iteration + 1, MAX_ITERATIONS);
        char* dev_msg = run_agent_loop(
            "Dev", C_DEV, run_id, 
            "You are a C developer. Write all source files and a Makefile for the project.\n"
            "Output format — use this EXACTLY:\n\n"
            "FILE:main.c\n"
            "#include <stdio.h>\n"
            "int main() { printf(\"hello\"); return 0; }\n"
            "ENDFILE\n\n"
            "FILE:Makefile\n"
            "all: main\n"
            "\tgcc -o main main.c\n"
            "ENDFILE\n\n"
            "DONE\n\n"
            "Output ALL files now. No explanation. No markdown.",
            dev_task
        );
        
        // Auto-compile and let Auditor verify
        ui_log(C_KING, "Kernel", "Running Auditor verification...");
        
        // Get workspace state and compile
        char compile_cmd[512];
        snprintf(compile_cmd, sizeof(compile_cmd), "playground/%s/run_cmd.sh", run_id);
        
        // Run make to get compile output for auditor
        char* ws_summary = NULL;
        {
            // Read workspace files for auditor context
            char dir_path[256];
            snprintf(dir_path, sizeof(dir_path), "playground/%s", run_id);
            // Simple file listing
            char listing[2048] = "";
            DIR *d = opendir(dir_path);
            if (d) {
                struct dirent *ent;
                int off = 0;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_type == DT_REG && strcmp(ent->d_name, "run_cmd.sh") != 0) {
                        off += snprintf(listing + off, sizeof(listing) - off, "  %s\n", ent->d_name);
                    }
                }
                closedir(d);
            }
            ws_summary = strdup(listing);
        }
        
        // Compile
        start_spinner("[System] Compiling...");
        char make_cmd[512];
        snprintf(make_cmd, sizeof(make_cmd), "cd playground/%s && make 2>&1", run_id);
        FILE* fp = popen(make_cmd, "r");
        char compile_out[2048] = "";
        if (fp) {
            char line[256];
            int off = 0;
            while (fgets(line, sizeof(line), fp) && off < 1800) {
                int n = snprintf(compile_out + off, sizeof(compile_out) - off, "%s", line);
                if (n > 0) off += n;
            }
            pclose(fp);
        }
        stop_spinner();
        if (strlen(compile_out) == 0) strcpy(compile_out, "(no output from make)");
        ui_log_raw(C_SYS, "-> Compile: %s", compile_out);
        
        // Auditor: single LLM call
        char* auditor_msg = run_auditor(run_id, compile_out, ws_summary);
        free(ws_summary);
        
        if (strstr(auditor_msg, "REJECT")) {
            ui_log(C_KING, "Kernel", "Rejected. Sending feedback to Dev.");
            free(dev_task);
            dev_task = malloc(strlen(auditor_msg) + strlen(prompt_buf) + strlen(compile_out) + 512);
            sprintf(dev_task, "Goal: %s\nREJECTED. Compile errors:\n%s\nAuditor says: %s\nFix and output corrected files.", 
                prompt_buf, compile_out, auditor_msg);
            iteration++;
        } else {
            ui_log(C_KING, "Kernel", "Consensus reached. Build verified.");
            consensus = 1;
        }
        
        free(dev_msg);
        free(auditor_msg);
    }
    
    if (!consensus) {
        ui_log(C_KING, "Kernel", "Max iterations reached (%d). Proceeding with best effort.", MAX_ITERATIONS);
    }
    free(dev_task);
    
    ui_log_raw(C_KING, "\n=============================================");
    ui_log_raw(C_KING, " INFINITE REPL SESSION ");
    ui_log_raw(C_KING, "=============================================\n");
    ui_log(C_KING, "Kernel", "Baseline verified. Interactive shell unlocked.");
    
    while(1) {
        char input[1024];
        ui_get_input("Command (or 'exit'): ", input, 1023);
        
        if (strcmp(input, "exit") == 0) break;
        if (strlen(input) == 0) continue;
        
        ui_log(C_YOU, "User", "%s", input);
        run_repl(run_id, input);
    }
    
    ui_log(C_KING, "Kernel", "Process terminated. Workspace intact at ./playground/%s", run_id);
    
    sleep(2);
    ui_destroy();
    
    printf("\033[2J\033[H\x1b[32mSilicon Court Run %s completed.\x1b[0m\n", run_id);
    return 0;
}
