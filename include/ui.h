#ifndef UI_H
#define UI_H

#define C_KING 1
#define C_DEV 2
#define C_HATER 3
#define C_QA 4
#define C_YOU 5
#define C_SYS 6

void ui_init(const char* run_id);
void ui_destroy(void);

// Logging functions
void ui_log(int color_pair, const char* role, const char* format, ...);
void ui_log_raw(int color_pair, const char* format, ...);

// Interactive input
void ui_get_input(const char* prompt, char* buffer, int max_len);

// Interactive menu
int ui_select_menu(const char* prompt, char** options, int num_options);

// State tracking updates
void ui_add_tokens(int count);
void ui_add_file_mod(void);

// Spinner for loading
void start_spinner(const char* label);
void stop_spinner(void);

#endif
