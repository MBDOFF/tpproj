#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void load_env(void) {
    FILE* f = fopen(".env", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* key = strtok(line, "=");
        char* val = strtok(NULL, "\n");
        if (key && val) {
            setenv(key, val, 1);
        }
    }
    fclose(f);
}
