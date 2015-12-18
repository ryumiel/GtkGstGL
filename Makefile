V ?= 0
V_GEN = $(V__gen_V_$(V))
V__gen_V_0 = @echo " GEN    " $@;
V__gen_V_1 =

V_CC = $(V__cc_V_$(V))
V__cc_V_0 = @echo " CC     " $@;
V__cc_V_1 =

V_LINK = $(V__link_V_$(V))
V__link_V_0 = @echo " LINK   " $@;
V__link_V_1 =

CC = gcc -std=c99
PKGCONFIG = $(shell which pkg-config)
CFLAGS = $(shell $(PKGCONFIG) --cflags gio-2.0 gtk+-3.0 gstreamer-1.0 epoxy) -g -O0
LIBS = -lGL $(shell $(PKGCONFIG) --libs gio-2.0 gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-plugins-bad-1.0 gstreamer-gl-1.0 epoxy) -lm
GLIB_COMPILE_RESOURCES = $(shell $(PKGCONFIG) --variable=glib_compile_resources gio-2.0)
GLIB_COMPILE_SCHEMAS = $(shell $(PKGCONFIG) --variable=glib_compile_schemas gio-2.0)

SRC = main.c
GEN =
BIN = glarea

ALL = $(GEN) $(SRC)
OBJS = $(ALL:.c=.o)

all: $(BIN)

%.o: %.c
	$(V_CC)$(CC) $(CFLAGS) -c -o $(@F) $<

$(BIN): $(OBJS)
	$(V_LINK)$(CC) -o $(@F) $(OBJS) $(LIBS)

clean:
	@rm -f $(GEN) $(OBJS) $(BIN)
