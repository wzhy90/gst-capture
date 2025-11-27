CC = clang
VERSION=1.0
TARGET = gst-capture-$(VERSION)
TARGET_DEBUG = $(TARGET)_debug
SRCS = main.c config.c recorder.c utils.c
PKG_LIBS = $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0) -liniparser
PKG_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0) -I/usr/include/iniparser
CFLAGS = $(PKG_CFLAGS) -O2
CFLAGS_DEBUG = $(PKG_CFLAGS) -g -DDEBUG
LIBS = $(PKG_LIBS)

.PHONY: all clean release debug

all: release debug

release: $(TARGET)

debug: $(TARGET_DEBUG)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(TARGET_DEBUG): $(SRCS)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGET) $(TARGET_DEBUG)
