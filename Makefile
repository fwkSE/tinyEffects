CC ?= cc
CFLAGS ?= -O2 -pipe
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
WARNFLAGS := -std=c99 -Wall -Wextra
LDLIBS := -ldl -lm

TARGET := tinyfx
SRC := src/main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
