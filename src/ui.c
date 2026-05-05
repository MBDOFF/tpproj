#include "ui.h"
#include <ncurses.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>

static WINDOW *win_top, *win_anim, *win_main_outer, *win_input;
static WINDOW *win_menu = NULL; 
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t ui_thread_id;

static volatile int ui_running = 1;
static volatile int spinner_active = 0;
static char current_label[256];
static char session_run_id[64];

// Global stats
static int g_tokens = 0;
static int g_files = 0;

// Input & Menu states
static volatile int input_active = 0;
static volatile int input_done = 0;
static char* input_buffer = NULL;
static int input_max = 0;
static int input_cursor = 0;
static const char* input_prompt = NULL;

static volatile int menu_active = 0;
static volatile int menu_done = 0;
static int menu_choice = 0;
static int highlight = 0;
static char** menu_options = NULL;
static int num_menu_options = 0;
static int text_len = 0;

static const char* cool_spinners[] = {
    "|", "/", "-", "\\"
};

// --- History Pad variables for scrolling ---
static WINDOW *pad_main = NULL;
static int pad_lines = 0;
static int pad_cols = 0;
static int pad_scroll_pos = 0;
static int max_lines = 10000;
static int view_h = 0;
static int view_w = 0;
static int view_y = 0;
static int view_x = 0;

static void refresh_pad(void) {
    int max_scroll = pad_lines - view_h;
    if (max_scroll < 0) max_scroll = 0;

    if (pad_scroll_pos < 0) {
        pad_scroll_pos = 0;
    } else if (pad_scroll_pos > max_scroll) {
        pad_scroll_pos = max_scroll;
    }
    
    prefresh(pad_main, pad_scroll_pos, 0, view_y, view_x, view_y + view_h - 1, view_x + view_w - 1);
}

static void draw_sidebar_stats(int frame) {
    wclear(win_anim);
    box(win_anim, 0, 0);
    mvwprintw(win_anim, 0, 2, " STATUS ");
    
    int color_idx = 10 + (frame % 6);
    wattron(win_anim, COLOR_PAIR(color_idx) | A_BOLD);
    
    if(spinner_active) {
        mvwprintw(win_anim, 2, 2, "[%s] RUNNING", cool_spinners[frame % 4]);
    } else {
        wattroff(win_anim, COLOR_PAIR(color_idx) | A_BOLD);
        wattron(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);
        mvwprintw(win_anim, 2, 2, "[+] IDLE");
        wattroff(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);
        wattron(win_anim, COLOR_PAIR(color_idx) | A_BOLD);
    }
    wattroff(win_anim, COLOR_PAIR(color_idx) | A_BOLD);

    double cost = (g_tokens / 1000000.0) * 0.70;
    
    wattron(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);
    mvwprintw(win_anim, 5, 2, "TELEMETRY ------------");
    wattroff(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);

    mvwprintw(win_anim, 7, 2, "TOKENS:");
    wattron(win_anim, COLOR_PAIR(C_YOU) | A_BOLD);
    mvwprintw(win_anim, 7, 10, "%d", g_tokens);
    wattroff(win_anim, COLOR_PAIR(C_YOU) | A_BOLD);

    mvwprintw(win_anim, 8, 2, "COST  :");
    wattron(win_anim, COLOR_PAIR(C_HATER) | A_BOLD);
    mvwprintw(win_anim, 8, 10, "$%.6f", cost);
    wattroff(win_anim, COLOR_PAIR(C_HATER) | A_BOLD);

    mvwprintw(win_anim, 9, 2, "FILES :");
    wattron(win_anim, COLOR_PAIR(C_DEV) | A_BOLD);
    mvwprintw(win_anim, 9, 10, "%d", g_files);
    wattroff(win_anim, COLOR_PAIR(C_DEV) | A_BOLD);

    wattron(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);
    mvwprintw(win_anim, 11, 2, "ACTION ---------------");
    wattroff(win_anim, COLOR_PAIR(C_SYS) | A_BOLD);
    
    wattron(win_anim, COLOR_PAIR(C_KING));
    mvwprintw(win_anim, 13, 2, "%.28s", current_label);
    wattroff(win_anim, COLOR_PAIR(C_KING));

    wrefresh(win_anim);
}

static int auto_scroll = 1;

static void handle_scroll(int ch) {
    int max_scroll = pad_lines - view_h;
    if (max_scroll < 0) max_scroll = 0;

    if (ch == KEY_UP || (!input_active && !menu_active && (ch == 'k' || ch == 3))) {
        if (pad_scroll_pos > 0) {
            pad_scroll_pos--;
            auto_scroll = 0;
        }
    } else if (ch == KEY_DOWN || (!input_active && !menu_active && (ch == 'j' || ch == 2))) {
        if (pad_scroll_pos < max_scroll) {
            pad_scroll_pos++;
        }
        if (pad_scroll_pos >= max_scroll) auto_scroll = 1;
    } else if (ch == KEY_PPAGE || ch == 21) {
        pad_scroll_pos -= view_h;
        if (pad_scroll_pos < 0) pad_scroll_pos = 0;
        auto_scroll = 0;
    } else if (ch == KEY_NPAGE || ch == 4) {
        pad_scroll_pos += view_h;
        if (pad_scroll_pos >= max_scroll) {
            pad_scroll_pos = max_scroll;
            auto_scroll = 1;
        }
    }
}

static void* ui_worker(void* arg) {
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    int tick = 0;
    
    while (ui_running) {
        pthread_mutex_lock(&ui_mutex);
        
        // 1. Process all pending keys instantly
        int ch;
        while ((ch = wgetch(stdscr)) != ERR) {
            // First check input/menu active state, otherwise handle scroll
            if (menu_active) {
                if (ch == KEY_UP || ch == 'k' || ch == 3) { highlight--; if(highlight < 0) highlight=0; }
                else if (ch == KEY_DOWN || ch == 'j' || ch == 2) { highlight++; if(highlight >= num_menu_options) highlight = num_menu_options-1; }
                else if (ch == '\n' || ch == '\r') { menu_done = 1; menu_choice = highlight; }
                // Let pgup/pgdn pass through to scroll logs behind menu
                else if (ch == KEY_PPAGE || ch == KEY_NPAGE) { handle_scroll(ch); }
            }
            else if (input_active) {
                if (ch == '\n' || ch == '\r') {
                    input_done = 1;
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                    if (input_cursor > 0) {
                        input_cursor--;
                        input_buffer[input_cursor] = '\0';
                    }
                } else if (isprint(ch) && input_cursor < input_max - 1) {
                    input_buffer[input_cursor++] = ch;
                    input_buffer[input_cursor] = '\0';
                }
                // Allow scrolling logs while typing
                else if (ch == KEY_UP || ch == KEY_DOWN || ch == KEY_PPAGE || ch == KEY_NPAGE || ch == 3 || ch == 2) {
                    handle_scroll(ch);
                }
            } 
            else {
                // Completely idle, all keys go to scroll
                handle_scroll(ch);
            }
        }
        
        // If menu was just marked done, exit early so we don't redraw it
        if (menu_active && menu_done) {
            // Skip redrawing
        }
        
        // 2. Draw Top Bar
        int w = getmaxx(stdscr);
        char pad_str[256];
        memset(pad_str, ' ', w);
        pad_str[w-1] = '\0';
        wattron(win_top, COLOR_PAIR(20));
        mvwprintw(win_top, 0, 0, "%s", pad_str);
        mvwprintw(win_top, 0, 1, " SILICON COURT | ID: %s | %s ", session_run_id, spinner_active ? "PROCESSING" : "IDLE");
        wattroff(win_top, COLOR_PAIR(20));
        wrefresh(win_top);

        // 3. Draw Sidebar (update frame 10 times a sec)
        if (tick % 6 == 0) {
            draw_sidebar_stats(tick / 6);
        }

        // 3.5. Ensure Main Outer Box is persistent
        touchwin(win_main_outer);
        wrefresh(win_main_outer);

        // 4. Draw Pad
        refresh_pad();
        
        // 5. Draw Active Overlays (Menu / Input)
        if (menu_active && win_menu && !menu_done) {
            for (int i = 0; i < num_menu_options; i++) {
                if (i == highlight) wattron(win_menu, A_REVERSE | COLOR_PAIR(C_YOU));
                mvwprintw(win_menu, i + 3, 2, " %s %-*.*s ", (i == highlight) ? ">" : " ", text_len, text_len, menu_options[i]);
                if (i == highlight) wattroff(win_menu, A_REVERSE | COLOR_PAIR(C_YOU));
            }
            wrefresh(win_menu);
        } 
        else if (input_active && !input_done) {
            wclear(win_input);
            box(win_input, 0, 0);
            wattron(win_input, COLOR_PAIR(C_YOU) | A_BOLD);
            mvwprintw(win_input, 1, 2, "%s", input_prompt);
            wattroff(win_input, COLOR_PAIR(C_YOU) | A_BOLD);
            
            mvwprintw(win_input, 1, 2 + strlen(input_prompt), "%s", input_buffer);
            wattron(win_input, A_REVERSE);
            mvwprintw(win_input, 1, 2 + strlen(input_prompt) + input_cursor, " ");
            wattroff(win_input, A_REVERSE);
            
            wrefresh(win_input);
        } 
        else {
            wclear(win_input);
            box(win_input, 0, 0);
            wattron(win_input, COLOR_PAIR(20));
            mvwprintw(win_input, 1, 2, " [ INPUT DISABLED - AGENTS WORKING ] ");
            wattroff(win_input, COLOR_PAIR(20));
            wrefresh(win_input);
        }
        
        pthread_mutex_unlock(&ui_mutex);
        usleep(16000); // 60 FPS
        tick++;
    }
    
    return NULL;
}

void ui_init(const char* run_id) {
    initscr();
    cbreak();
    noecho();
    start_color();
    use_default_colors();
    curs_set(0);

    init_pair(C_KING, COLOR_YELLOW, -1);
    init_pair(C_DEV, COLOR_GREEN, -1);
    init_pair(C_HATER, COLOR_RED, -1);
    init_pair(C_QA, COLOR_BLUE, -1);
    init_pair(C_YOU, COLOR_CYAN, -1);
    init_pair(C_SYS, COLOR_WHITE, -1);

    init_pair(10, COLOR_RED, -1);
    init_pair(11, COLOR_YELLOW, -1);
    init_pair(12, COLOR_GREEN, -1);
    init_pair(13, COLOR_CYAN, -1);
    init_pair(14, COLOR_BLUE, -1);
    init_pair(15, COLOR_MAGENTA, -1);

    init_pair(20, COLOR_BLACK, COLOR_WHITE); 
    init_pair(21, COLOR_WHITE, COLOR_BLUE);

    strncpy(session_run_id, run_id, sizeof(session_run_id)-1);
    strncpy(current_label, "Awaiting input...", sizeof(current_label)-1);

    int h, w;
    getmaxyx(stdscr, h, w);

    win_top = newwin(1, w, 0, 0);
    win_anim = newwin(h - 4, 32, 1, 0);
    win_main_outer = newwin(h - 4, w - 32, 1, 32);
    box(win_main_outer, 0, 0);
    mvwprintw(win_main_outer, 0, 2, " OPERATION LOGS ");
    wrefresh(win_main_outer);

    view_h = h - 5;
    view_w = w - 36;
    if (view_w < 10) view_w = 10;
    if (view_h < 5) view_h = 5;
    view_y = 2;
    view_x = 34;
    pad_cols = view_w;
    
    pad_main = newpad(max_lines, pad_cols);
    win_input = newwin(3, w, h - 3, 0);
    
    // Spawn optimized 60fps dedicated UI thread
    ui_running = 1;
    pthread_create(&ui_thread_id, NULL, ui_worker, NULL);
}

void ui_destroy(void) {
    ui_running = 0;
    pthread_join(ui_thread_id, NULL);
    if (pad_main) delwin(pad_main);
    endwin();
}

void ui_add_tokens(int count) {
    pthread_mutex_lock(&ui_mutex);
    g_tokens += count;
    pthread_mutex_unlock(&ui_mutex);
}

void ui_add_file_mod(void) {
    pthread_mutex_lock(&ui_mutex);
    g_files++;
    pthread_mutex_unlock(&ui_mutex);
}

static void typewriter_print(int color_pair, const char* text) {
    int len = strlen(text);
    int chunk_size = 5; 
    
    for (int i = 0; i < len; i += chunk_size) {
        pthread_mutex_lock(&ui_mutex);
        wattron(pad_main, COLOR_PAIR(color_pair));
        for (int j = 0; j < chunk_size && i + j < len; j++) {
            waddch(pad_main, text[i + j]);
        }
        wattroff(pad_main, COLOR_PAIR(color_pair));
        
        int y, x;
        getyx(pad_main, y, x);
        pad_lines = y + 1;
        
        int max_scroll = pad_lines - view_h;
        if (max_scroll < 0) max_scroll = 0;
        
        if (auto_scroll) pad_scroll_pos = max_scroll;
        
        refresh_pad();
        pthread_mutex_unlock(&ui_mutex);
        
        // Removed sleep to fix typewriter hang and make UI instantly fast
        // usleep(3000); 
    }
}

void ui_log(int color_pair, const char* role, const char* format, ...) {
    char buf[16384];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    char header[256];
    snprintf(header, sizeof(header), "[%s] ", role);
    
    pthread_mutex_lock(&ui_mutex);
    wattron(pad_main, COLOR_PAIR(color_pair) | A_BOLD);
    wprintw(pad_main, "%s", header);
    wattroff(pad_main, COLOR_PAIR(color_pair) | A_BOLD);
    pthread_mutex_unlock(&ui_mutex);

    // Typewriter output
    typewriter_print(color_pair, buf);

    // Add trailing newlines
    pthread_mutex_lock(&ui_mutex);
    waddch(pad_main, '\n');
    waddch(pad_main, '\n');
    int y, x;
    getyx(pad_main, y, x);
    pad_lines = y + 1;
    
    int max_scroll = pad_lines - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (auto_scroll) pad_scroll_pos = max_scroll;
    
    refresh_pad();
    pthread_mutex_unlock(&ui_mutex);
}

void ui_log_raw(int color_pair, const char* format, ...) {
    char buf[16384];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    // Typewriter output
    typewriter_print(color_pair, buf);

    pthread_mutex_lock(&ui_mutex);
    waddch(pad_main, '\n');
    int y, x;
    getyx(pad_main, y, x);
    pad_lines = y + 1;
    
    int max_scroll = pad_lines - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (auto_scroll) pad_scroll_pos = max_scroll;
    
    refresh_pad();
    pthread_mutex_unlock(&ui_mutex);
}

void ui_get_input(const char* prompt, char* buffer, int max_len) {
    pthread_mutex_lock(&ui_mutex);
    input_prompt = prompt;
    input_buffer = buffer;
    input_max = max_len;
    input_cursor = 0;
    input_buffer[0] = '\0';
    input_done = 0;
    input_active = 1;
    pthread_mutex_unlock(&ui_mutex);

    while (!input_done) {
        usleep(10000); 
    }

    pthread_mutex_lock(&ui_mutex);
    input_active = 0;
    pthread_mutex_unlock(&ui_mutex);
}

int ui_select_menu(const char* prompt, char** options, int num_options) {
    pthread_mutex_lock(&ui_mutex);
    int h, w;
    getmaxyx(stdscr, h, w);
    
    int menu_h = num_options + 4;
    int menu_w = w - 38; 
    if (menu_w < 40) menu_w = w - 2;
    if (menu_w > 110) menu_w = 110;
    int start_y = (h - menu_h) / 2;
    if (start_y < 0) start_y = 0;
    int start_x = 34 + ((w - 34 - menu_w) / 2);
    if (start_x + menu_w >= w) {
        start_x = w - menu_w - 1;
        if (start_x < 0) start_x = 0;
    }

    text_len = menu_w - 7;
    if (text_len < 10) text_len = 10;

    win_menu = newwin(menu_h, menu_w, start_y, start_x);
    if (win_menu) {
        box(win_menu, 0, 0);
        wattron(win_menu, COLOR_PAIR(C_YOU) | A_BOLD);
        mvwprintw(win_menu, 1, 2, "%.*s", menu_w - 4, prompt);
        wattroff(win_menu, COLOR_PAIR(C_YOU) | A_BOLD);
    }
    
    menu_options = options;
    num_menu_options = num_options;
    highlight = 0;
    menu_done = 0;
    menu_active = 1;
    pthread_mutex_unlock(&ui_mutex);

    // Force a UI tick to draw the menu immediately
    usleep(50000);

    while (!menu_done) {
        usleep(10000); 
    }

    pthread_mutex_lock(&ui_mutex);
    menu_active = 0;
    int final_choice = menu_choice;
    if (win_menu) {
        delwin(win_menu);
        win_menu = NULL;
    }
    touchwin(win_main_outer); 
    pthread_mutex_unlock(&ui_mutex);

    return final_choice;
}

void start_spinner(const char* label) {
    pthread_mutex_lock(&ui_mutex);
    strncpy(current_label, label, sizeof(current_label)-1);
    spinner_active = 1;
    pthread_mutex_unlock(&ui_mutex);
}

void stop_spinner(void) {
    pthread_mutex_lock(&ui_mutex);
    spinner_active = 0;
    strncpy(current_label, "Awaiting input...", sizeof(current_label)-1);
    pthread_mutex_unlock(&ui_mutex);
    
    // Explicitly wait for the spinner thread to finish its current sleep loop
    // so it doesn't accidentally overwrite the UI state when we transition.
    usleep(150000); 
}
