CC=clang
CFLAGS=-Weverything -Wno-padded -Wno-missing-prototypes -Wno-format-nonliteral -Werror -std=gnu99 -ggdb -I. -pipe -IBlocksRuntime/BlocksRuntime -D$(shell uname | tr '[:lower:]' '[:upper:]')
LDFLAGS=-pipe

# Not the best but should work
IS_DARWIN=$(shell (uname -a | grep -q -i darwin) && echo 1 || echo 0)

SRC_CLIENT=client/main.c client/Client.c utils/object.c utils/helper.c net/poll.c utils/dispatchqueue.c utils/queue.c
OBJS_CLIENT=$(SRC_CLIENT:.c=.o) BlocksRuntime/libBlocksRuntime.a

SRC_SERVER=server/main.c server/server.c server/listener.c server/client.c utils/object.c utils/helper.c utils/str_helper.c utils/dictionary.c utils/stack.c net/poll.c utils/dispatchqueue.c utils/queue.c
OBJS_SERVER=$(SRC_SERVER:.c=.o) BlocksRuntime/libBlocksRuntime.a

ifneq ($(IS_DARWIN), 1)
CFLAGS+=-pthread -fblocks
LDFLAGS+=-pthread
else
LDFLAGS+=-framework Cocoa
endif

# Makefile dependency to recompile when flags changed
%.o: %.c Makefile
	@echo "[CC] $@"
	@$(CC) $(CFLAGS) -MD -c -o $@ $<

%.E: %.c
	@echo "[PRE] $@"
	@$(CC) $(CFLAGS) -E -o $@ $<

all: chatclient chatserver

chatserver: $(OBJS_SERVER)
	@echo "[LD] $@"
	@$(CC) $(LDFLAGS) -o $@ $^
	
chatclient: $(OBJS_CLIENT)
	@echo "[LD] $@"
	$(CC) $(LDFLAGS) -o $@ $^  gui-lib/libchatgui64.a `pkg-config --libs gtk+-2.0`

BlocksRuntime/libBlocksRuntime.a:
	cd BlocksRuntime && ./buildlib

clean:
	rm -f webserver
	rm -f BlocksRuntime/libBlocksRuntime.a
	rm -f $(OBJS_CLIENT) $(OBJS_SERVER)
	rm -f $(OBJS_CLIENT:.o=.d) $(OBJS_SERVER:.o=.d)
	
-include $(SRC_CLIENT:.c=.d) $(SRC_SERVER:.c=.d)
