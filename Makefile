# Ttabmux - Terminal multiplexer with sidebar
# MIT License

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c99 -O2
LDFLAGS  ?=
LIBS     = -lutil

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

SRC_DIR  = src
BUILD_DIR = build

SRCS     = $(SRC_DIR)/main.c $(SRC_DIR)/vterm.c $(SRC_DIR)/render.c
OBJS     = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET   = ttabmux

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/ttabmux.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
