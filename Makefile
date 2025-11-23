CC = clang
TARGET = ns
SRCS = main.c config.c
PKG_LIBS = $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0 iniparser)
PKG_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0 iniparser)
CFLAGS = $(PKG_CFLAGS)
LIBS = $(PKG_LIBS)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGET)
