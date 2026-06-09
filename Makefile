# JackDAW — plain Makefile (no autotools)
# Usage:  make [-j$(nproc)]   build
#         make clean           remove build artifacts
#
SRCDIR  := src
TARGET  := $(SRCDIR)/jackdaw

CC      := gcc

# ---------------------------------------------------------------------------
# Package detection via pkg-config
# ---------------------------------------------------------------------------

PKGS_REQ := gtk+-3.0 jack sndfile

HAS_ALSA := $(shell pkg-config --exists alsa       2>/dev/null && echo 1)
HAS_SR   := $(shell pkg-config --exists samplerate 2>/dev/null && echo 1)
HAS_LILV := $(shell pkg-config --exists lilv-0     2>/dev/null && echo 1)
HAS_SUIL := $(shell pkg-config --exists suil-0     2>/dev/null && echo 1)

PKGS_OPT :=
OPT_DEFS :=
ifneq ($(HAS_ALSA),)
PKGS_OPT += alsa
OPT_DEFS += -DHAVE_ALSA=1
endif
ifneq ($(HAS_SR),)
PKGS_OPT += samplerate
OPT_DEFS += -DHAVE_SAMPLERATE=1
endif
ifneq ($(HAS_LILV),)
PKGS_OPT += lilv-0
OPT_DEFS += -DHAVE_LV2=1
endif
ifneq ($(HAS_SUIL),)
PKGS_OPT += suil-0
OPT_DEFS += -DHAVE_SUIL=1
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS_REQ) $(PKGS_OPT))
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS_REQ) $(PKGS_OPT))

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

CFLAGS := \
    -g -O2 \
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
    -std=gnu99 \
    -I$(SRCDIR) \
    $(PKG_CFLAGS) \
    $(OPT_DEFS)

LDFLAGS := \
    $(PKG_LIBS) \
    -lm -lpthread -ldl

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRCS := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/settings.c \
    $(SRCDIR)/um.c \
    $(SRCDIR)/audio_clip.c \
    $(SRCDIR)/track.c \
    $(SRCDIR)/project.c \
    $(SRCDIR)/jackdaw-engine.c \
    $(SRCDIR)/timeline.c \
    $(SRCDIR)/mainwindow.c

OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@
	@echo "Built: $@"

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d $(TARGET)
