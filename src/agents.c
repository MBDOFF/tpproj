#include "agents.h"
#include "llm.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_AGENT_STEPS 6
#define CMD_TIMEOUT_SEC 5
#define MAX_FILE_BYTES 2048
#define WS_BUFFER_SIZE (512 * 1024)
#define CTX_BUFFER_SIZE 16384
#define CMD_OUTPUT_SIZE 4096

// ============================================================
// WORKSPACE READER
// ============================================================
static char* get_workspace_files(const char* run_id) {
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "playground/%s", run_id);
    DIR *d = opendir(dir_path);
    if (!d) return strdup("(empty workspace)");

    char* out = malloc(WS_BUFFER_SIZE);
    if (!out) return strdup("(error)");
    int offset = 0;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) {
            if (strstr(dir->d_name, ".c") || strstr(dir->d_name, ".h") || 
                strstr(dir->d_name, "Makefile") || strstr(dir->d_name, ".sh")) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, dir->d_name);
                FILE* f = fopen(filepath, "r");
                if (f) {
                    offset += snprintf(out + offset, WS_BUFFER_SIZE - offset,
                        "\n--- %s ---\n", dir->d_name);
                    int file_bytes = 0;
                    char buf[1024];
                    while (fgets(buf, sizeof(buf), f) && file_bytes < MAX_FILE_BYTES) {
                        int remaining = WS_BUFFER_SIZE - offset - 100;
                        if (remaining <= 0) break;
                        int n = snprintf(out + offset, remaining, "%s", buf);
                        if (n > 0) { offset += n; file_bytes += n; }
                    }
                    if (file_bytes >= MAX_FILE_BYTES)
                        offset += snprintf(out + offset, WS_BUFFER_SIZE - offset, "\n...[truncated]...\n");
                    fclose(f);
                    offset += snprintf(out + offset, WS_BUFFER_SIZE - offset, "\n");
                }
            }
        }
    }
    closedir(d);
    out[offset] = '\0';
    return out;
}

// ============================================================
// SAFE BASH EXECUTION (fork + timeout)
// ============================================================
static char* exec_cmd(const char* run_id, const char* cmd_str) {
    char script_path[512];
    snprintf(script_path, sizeof(script_path), "playground/%s/run_cmd.sh", run_id);
    FILE* sf = fopen(script_path, "w");
    if (!sf) return strdup("(error: could not write script)");
    fprintf(sf, "%s\n", cmd_str);
    fclose(sf);

    int pipefd[2];
    if (pipe(pipefd) == -1) { unlink(script_path); return strdup("(error: pipe)"); }

    pid_t pid = fork();
    if (pid == -1) { close(pipefd[0]); close(pipefd[1]); unlink(script_path); return strdup("(error: fork)"); }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        close(STDIN_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        char dp[256];
        snprintf(dp, sizeof(dp), "playground/%s", run_id);
        if (chdir(dp) != 0) _exit(1);
        alarm(CMD_TIMEOUT_SEC);
        execlp("bash", "bash", "run_cmd.sh", (char*)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    char* output = calloc(1, CMD_OUTPUT_SIZE);
    int output_len = 0;
    int status, child_done = 0, elapsed_ms = 0;

    while (elapsed_ms < (CMD_TIMEOUT_SEC + 1) * 1000) {
        char buf[512]; ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            int space = CMD_OUTPUT_SIZE - output_len - 1;
            if (space > 0) { int c = (n<space)?(int)n:space; memcpy(output+output_len,buf,c); output_len+=c; output[output_len]='\0'; }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            child_done = 1;
            while ((n = read(pipefd[0], buf, sizeof(buf)-1)) > 0) {
                buf[n]='\0'; int space=CMD_OUTPUT_SIZE-output_len-1;
                if(space>0){int c=(n<space)?(int)n:space;memcpy(output+output_len,buf,c);output_len+=c;output[output_len]='\0';}
            }
            break;
        }
        usleep(50000); elapsed_ms += 50;
    }

    if (!child_done) { kill(pid, SIGKILL); waitpid(pid, &status, 0); snprintf(output, CMD_OUTPUT_SIZE, "(killed: timeout %ds)", CMD_TIMEOUT_SEC); }
    else if (output_len == 0) strcpy(output, "(ok, no output)");

    close(pipefd[0]);
    unlink(script_path);
    return output;
}

// ============================================================
// COMPILE THE PROJECT (auto-runs make)
// ============================================================
static char* compile_project(const char* run_id) {
    ui_log(C_SYS, "System", "Auto-compiling...");
    char* output = exec_cmd(run_id, "make 2>&1");
    // Truncate for display
    if (strlen(output) > 300) output[300] = '\0';
    ui_log_raw(C_SYS, "-> %s", output);
    return output;
}

// ============================================================
// PARSE FILES FROM RESPONSE
// Supports both formats:
//   FORMAT A: FILE:name.c\ncontent\nENDFILE
//   FORMAT B: <write file="name.c">content</write>  (fallback)
// Returns number of files written
// ============================================================
static int parse_and_write_files(const char* response, const char* run_id, const char* role_name, int color_id) {
    int files_written = 0;
    
    // === FORMAT A: FILE:name / ENDFILE ===
    {
        const char* scan = response;
        while (1) {
            const char* marker = strstr(scan, "FILE:");
            if (!marker) break;
            // Make sure it's at start of line or start of text
            if (marker != response && *(marker-1) != '\n') { scan = marker + 5; continue; }
            
            const char* name_start = marker + 5;
            const char* name_end = strchr(name_start, '\n');
            if (!name_end) break;
            
            char fname[256];
            int name_len = name_end - name_start;
            if (name_len > 255) name_len = 255;
            // Trim whitespace from filename
            while (name_len > 0 && (name_start[name_len-1] == ' ' || name_start[name_len-1] == '\r')) name_len--;
            strncpy(fname, name_start, name_len);
            fname[name_len] = '\0';
            
            // Skip empty filenames or "name.c" placeholder
            if (strlen(fname) == 0) { scan = name_end + 1; continue; }
            
            const char* content_start = name_end + 1;
            const char* end_marker = strstr(content_start, "ENDFILE");
            if (!end_marker) break;
            
            int content_len = end_marker - content_start;
            // Trim trailing newline before ENDFILE
            if (content_len > 0 && content_start[content_len-1] == '\n') content_len--;
            
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "playground/%s/%s", run_id, fname);
            FILE* f = fopen(filepath, "w");
            if (f) {
                fwrite(content_start, 1, content_len, f);
                fclose(f);
                ui_add_file_mod();
                ui_log(color_id, role_name, "Wrote: %s", fname);
                files_written++;
            }
            
            scan = end_marker + 7; // past "ENDFILE"
        }
    }
    
    // === FORMAT B (fallback): <write file="name">content</write> ===
    if (files_written == 0) {
        char* resp_copy = strdup(response);
        char* scan = resp_copy;
        while (1) {
            char* ws = strstr(scan, "<write file=\"");
            if (!ws) break;
            ws += 13;
            char* name_end = strstr(ws, "\">");
            if (!name_end) break;
            *name_end = '\0';
            char fname[256];
            strncpy(fname, ws, sizeof(fname)-1); fname[sizeof(fname)-1] = '\0';
            
            char* content_start = name_end + 2;
            if (*content_start == '\n') content_start++;
            char* write_end = strstr(content_start, "</write>");
            if (!write_end) break;
            *write_end = '\0';
            
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "playground/%s/%s", run_id, fname);
            FILE* f = fopen(filepath, "w");
            if (f) {
                fputs(content_start, f);
                fclose(f);
                ui_add_file_mod();
                ui_log(color_id, role_name, "Wrote: %s", fname);
                files_written++;
            }
            scan = write_end + 8;
        }
        free(resp_copy);
    }
    
    return files_written;
}

// ============================================================
// DEV AGENT LOOP
// Simplified: write files → auto-compile → if error, feed back → repeat
// ============================================================
char* run_agent_loop(const char* role_name, int color_id, const char* run_id, const char* system_prompt, const char* task) {
    char* context = calloc(1, CTX_BUFFER_SIZE);
    snprintf(context, CTX_BUFFER_SIZE, "Task: %s\n", task);
    char* final_result = NULL;
    int step = 0;
    
    while (step < MAX_AGENT_STEPS) {
        step++;
        char* ws_cache = get_workspace_files(run_id);
        
        size_t prompt_size = strlen(context) + strlen(ws_cache) + strlen(system_prompt) + 2048;
        char* llm_prompt = malloc(prompt_size);
        
        snprintf(llm_prompt, prompt_size,
            "%s\n\n"
            "=== CURRENT FILES ===\n%s\n=====================\n\n"
            "To create/overwrite files, output:\nFILE:filename.c\ncode here\nENDFILE\n\n"
            "You can output MULTIPLE files. After files, output:\nDONE\n\n"
            "Output files NOW. No explanation. No markdown.\n\n"
            "%s\n\n%s:",
            system_prompt, ws_cache, context, role_name);
        
        free(ws_cache);
        
        char spinner_label[128];
        snprintf(spinner_label, sizeof(spinner_label), "[%s] Step %d/%d...", role_name, step, MAX_AGENT_STEPS);
        start_spinner(spinner_label);
        
        int tokens_used = 0;
        char* response = query_gemini(llm_prompt, &tokens_used);
        
        stop_spinner();
        ui_add_tokens(tokens_used);
        free(llm_prompt);
        
        // Parse and write files from the response
        int files_written = parse_and_write_files(response, run_id, role_name, color_id);
        
        if (files_written > 0) {
            // Auto-compile!
            char* compile_out = compile_project(run_id);
            
            // Check if compilation succeeded (no "error:" in output)
            int has_error = (strstr(compile_out, "error:") || strstr(compile_out, "Error ") || 
                           strstr(compile_out, "undefined reference") || strstr(compile_out, "No rule to make"));
            
            if (!has_error) {
                // Success!
                ui_log(color_id, role_name, "Code compiles successfully.");
                final_result = strdup("Code written and compiles successfully.");
                free(compile_out);
                free(response);
                break;
            } else {
                // Feed compile errors back
                ui_log(C_SYS, "System", "Compile errors found. Feeding back to %s...", role_name);
                
                // Compact context if needed
                int ctx_len = strlen(context);
                if (ctx_len > CTX_BUFFER_SIZE - 2048) {
                    int keep = CTX_BUFFER_SIZE / 2;
                    memmove(context, context + (ctx_len - keep), keep + 1);
                }
                
                char summary[1024];
                snprintf(summary, sizeof(summary), 
                    "Compile errors:\n%.500s\nFix these errors and output corrected files.\n", compile_out);
                strncat(context, summary, CTX_BUFFER_SIZE - strlen(context) - 1);
            }
            free(compile_out);
        } else {
            // No files found — check for DONE/message/plain text completion
            if (strstr(response, "DONE") || strstr(response, "done") ||
                strstr(response, "<message>") || strstr(response, "APPROVE") || 
                strstr(response, "REJECT") || strstr(response, "completed")) {
                char result_buf[501];
                snprintf(result_buf, sizeof(result_buf), "%.500s", response);
                ui_log(color_id, role_name, "%s", result_buf);
                final_result = strdup(result_buf);
                free(response);
                break;
            }
            
            // Truly invalid — log preview and retry
            char preview[201];
            snprintf(preview, sizeof(preview), "%.200s", response);
            ui_log_raw(C_SYS, "-> No files found in output (%d/%d): %s", step, MAX_AGENT_STEPS, preview);
            strncat(context, "System: Invalid response. Output files using:\nFILE:name.c\ncode\nENDFILE\n",
                CTX_BUFFER_SIZE - strlen(context) - 1);
            usleep(300000);
        }
        
        free(response);
    }
    
    if (!final_result) {
        ui_log_raw(C_SYS, "-> %s hit step limit (%d).", role_name, MAX_AGENT_STEPS);
        final_result = strdup("(step limit reached)");
    }
    
    free(context);
    return final_result;
}

// ============================================================
// AUDITOR: Single LLM call, NO tools
// Just looks at compile output + workspace and says APPROVE/REJECT
// ============================================================
char* run_auditor(const char* run_id, const char* compile_output, const char* workspace_summary) {
    char prompt[8192];
    snprintf(prompt, sizeof(prompt),
        "You are a code auditor. A developer wrote C code. Below is the compilation output and the source files.\n\n"
        "COMPILATION OUTPUT:\n%s\n\n"
        "SOURCE FILES:\n%s\n\n"
        "Rules:\n"
        "- If compilation has 0 errors, respond with exactly: APPROVE\n"
        "- If there are errors, respond with: REJECT: [one line describing the issue]\n"
        "Respond with ONLY one word (APPROVE) or REJECT: reason. Nothing else.",
        compile_output, workspace_summary);
    
    start_spinner("[Auditor] Reviewing...");
    int tokens_used = 0;
    char* response = query_gemini(prompt, &tokens_used);
    stop_spinner();
    ui_add_tokens(tokens_used);
    
    // Clean up: trim and truncate
    char* trimmed = response;
    while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') trimmed++;
    
    char result[512];
    snprintf(result, sizeof(result), "%.500s", trimmed);
    
    ui_log(C_HATER, "Auditor", "%s", result);
    
    char* ret = strdup(result);
    free(response);
    return ret;
}

// ============================================================
// REPL: Single LLM call for interactive session
// Handles both text questions and file modifications
// ============================================================
void run_repl(const char* run_id, const char* user_input) {
    char* ws_cache = get_workspace_files(run_id);
    
    size_t prompt_size = strlen(ws_cache) + strlen(user_input) + 2048;
    char* prompt = malloc(prompt_size);
    
    snprintf(prompt, prompt_size,
        "You are a C developer assistant chatting with a user about their project.\n\n"
        "=== PROJECT FILES ===\n%s\n=====================\n\n"
        "User: %s\n\n"
        "RULES:\n"
        "1. If the user asks a QUESTION (what, why, how, explain, is, does, can, etc.) — respond with ONLY text. Do NOT output any FILE: blocks. Just answer the question.\n"
        "2. ONLY if the user EXPLICITLY asks to change/fix/add/modify/create code, output the modified files using:\n"
        "FILE:filename.c\ncode\nENDFILE\n\n"
        "Default to text answers. Only write files when explicitly asked.\n\n"
        "Respond:",
        ws_cache, user_input);
    
    free(ws_cache);
    
    start_spinner("[Dev] Thinking...");
    int tokens_used = 0;
    char* response = query_gemini(prompt, &tokens_used);
    stop_spinner();
    ui_add_tokens(tokens_used);
    free(prompt);
    
    // Try to parse files from response
    int files_written = parse_and_write_files(response, run_id, "Dev", C_DEV);
    
    if (files_written > 0) {
        // Auto-compile after modifications
        char* compile_out = compile_project(run_id);
        free(compile_out);
    }
    
    // Always show the text response (trim file content out for readability)
    // Find text before first FILE: or after last ENDFILE, or the whole thing

    
    // If there are FILE: blocks, show text around them

    char* last_endfile = NULL;
    {
        char* scan = response;
        while (1) {
            char* ef = strstr(scan, "ENDFILE");
            if (!ef) break;
            last_endfile = ef + 7;
            scan = last_endfile;
        }
    }
    
    if (files_written > 0 && last_endfile && *last_endfile) {
        // Show text after the last ENDFILE
        char* after = last_endfile;
        while (*after == '\n' || *after == '\r' || *after == ' ') after++;
        if (strlen(after) > 2) {
            ui_log(C_DEV, "Dev", "%s", after);
        } else {
            ui_log(C_DEV, "Dev", "Files updated.");
        }
    } else if (files_written == 0) {
        // Pure text response — show it all (truncated)
        char* trimmed = response;
        while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') trimmed++;
        if (strlen(trimmed) > 1500) trimmed[1500] = '\0';
        ui_log(C_DEV, "Dev", "%s", trimmed);
    }
    
    free(response);
}
