#ifndef AGENTS_H
#define AGENTS_H

// Dev agent: writes files, auto-compiles, fixes errors
char* run_agent_loop(const char* role_name, int color_id, const char* run_id, const char* system_prompt, const char* task);

// Auditor: single LLM call, no tools — just reads compile output and says APPROVE/REJECT
char* run_auditor(const char* run_id, const char* compile_output, const char* workspace_summary);

// REPL: handles both text questions AND file modifications in one call
void run_repl(const char* run_id, const char* user_input);

#endif
