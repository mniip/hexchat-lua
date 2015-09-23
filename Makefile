CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -ggdb

CFLAGS += -std=c99 -fPIC -Wall -Wextra -pedantic
CFLAGS += -Wno-unused-parameter

CFLAGS += $(shell $(PKG_CONFIG) --cflags hexchat-plugin)
CFLAGS += $(shell $(PKG_CONFIG) --cflags lua)
CFLAGS += $(shell $(PKG_CONFIG) --cflags glib-2.0)

LDFLAGS ?= -shared
LIBS += $(shell $(PKG_CONFIG) --libs lua)
LIBS += $(shell $(PKG_CONFIG) --libs glib-2.0)

OUTPUT = lua.so

FILES = lua.c

all:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUTPUT) $(FILES) $(LIBS)

clean:
	rm -f $(OUTPUT)
