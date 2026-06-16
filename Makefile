CC ?= cc
CFLAGS ?= -Os -pipe
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
WARNFLAGS := -std=c99 -Wall -Wextra
LDLIBS := -lX11 -ldl -lm

TARGET := tinyfx
SRC := src/main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
