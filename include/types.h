#ifndef TYPES_H
#define TYPES_H
#include <sys/types.h>

typedef enum { MSG_TASK, MSG_CODE, MSG_CRITIQUE, MSG_APPROVAL, MSG_FORCE_CONSENSUS, MSG_TERMINATE } MsgType;

typedef struct {
    int to_child[2];
    int from_child[2];
    pid_t pid;
} AgentConnection;

typedef struct {
    char prompt[4096];
    char run_id[32];
    int iteration;
    int completed;
    int onboarded;
} SessionState;

#endif
