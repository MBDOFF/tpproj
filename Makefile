CC = gcc
CFLAGS = -Wall -Iinclude -g
SRC = src/main.c src/agents.c src/cJSON.c src/llm.c src/env.c src/ui.c
OBJ = $(SRC:.c=.o)
EXEC = silicon_court
LIBS = -lcurl -lpthread -lncurses

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(EXEC) state.bin
	rm -rf output playground setup_playground.sh
