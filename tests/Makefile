CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
INCLUDES = -I../firmware/shared/include
SRC = ../firmware/shared/src/protocol.c
TEST = test_protocol

all: $(TEST)

$(TEST): test_protocol.c $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SRC) -o $@

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(TEST)

.PHONY: all test clean
