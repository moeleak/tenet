CC ?= cc
BIN := tenet
BOT_BIN := tenet-bot

SERVER_SRC := src/main.c src/server.c src/server_auth.c src/server_accounts.c src/session.c src/ldap.c
BOT_SRC := src/tenet_bot.c src/bot_config.c src/bot_http.c src/bot_memory.c src/bot_ollama.c src/bot_protocol.c src/bot_screen.c src/bot_util.c
SERVER_OBJ := $(SERVER_SRC:.c=.o)
BOT_OBJ := $(BOT_SRC:.c=.o)
SERVER_HEADERS := src/tenet.h src/server_internal.h
BOT_HEADERS := src/bot_config.h src/bot_http.h src/bot_memory.h src/bot_ollama.h src/bot_protocol.h src/bot_screen.h src/bot_util.h

CPPFLAGS ?=
CPPFLAGS += -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS += -pthread
BOT_LDLIBS ?= -lsqlite3 -lm

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall run bot

all: $(BIN) $(BOT_BIN)

$(BIN): $(SERVER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(SERVER_OBJ) $(LDLIBS)

$(BOT_BIN): $(BOT_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(BOT_OBJ) $(BOT_LDLIBS)

$(SERVER_OBJ): $(SERVER_HEADERS)
$(BOT_OBJ): $(BOT_HEADERS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

bot: $(BOT_BIN)

run: $(BIN)
	./$(BIN)

install: $(BIN) $(BOT_BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 0755 $(BOT_BIN) $(DESTDIR)$(BINDIR)/$(BOT_BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) $(DESTDIR)$(BINDIR)/$(BOT_BIN)

clean:
	rm -f $(SERVER_OBJ) $(BOT_OBJ) $(BIN) $(BOT_BIN)
