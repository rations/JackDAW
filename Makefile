# JackDAW — plain Makefile (no autotools)
# Usage:  make [-j$(nproc)]   build
#         make clean           remove build artifacts
#         make VST3=1          also build the VST3 backend (needs the SDK)
#
SRCDIR  := src
EXTDIR  := ext
TARGET  := $(SRCDIR)/jackdaw

.DEFAULT_GOAL := all

CC      := gcc
CXX     := g++

VST3SDK ?= /home/human/third_party/vst3sdk
# VST3 backend is built by default (you have the SDK). Disable with: make VST3=0
VST3    ?= 1

# ---------------------------------------------------------------------------
# Package detection via pkg-config
# ---------------------------------------------------------------------------

PKGS_REQ := gtk+-3.0 gtk+-x11-3.0 jack sndfile

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

# VST2, CLAP and LADSPA backends use vendored headers in ext/ — always available.
OPT_DEFS += -DHAVE_VST2=1 -DHAVE_CLAP=1 -DHAVE_LADSPA=1

# VST3 enable must be decided BEFORE COMMON captures OPT_DEFS.
VST3_INC :=
ifeq ($(VST3),1)
OPT_DEFS += -DHAVE_VST3=1
VST3_INC := -I$(VST3SDK) -DRELEASE=1
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS_REQ) $(PKGS_OPT))
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS_REQ) $(PKGS_OPT))

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
COMMON := -g -O2 $(WARN) -I$(SRCDIR) -I$(EXTDIR) $(PKG_CFLAGS) $(OPT_DEFS)

# make ASAN=1  -> build with AddressSanitizer to pinpoint memory corruption.
ifeq ($(ASAN),1)
COMMON += -fsanitize=address -fno-omit-frame-pointer -O1
endif

CFLAGS   := $(COMMON) -std=gnu99
CXXFLAGS := $(COMMON) -std=c++17 $(VST3_INC)

LDFLAGS := \
    $(PKG_LIBS) \
    -lm -lpthread -ldl -lstdc++
ifeq ($(ASAN),1)
LDFLAGS += -fsanitize=address
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRCS_C := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/settings.c \
    $(SRCDIR)/message.c \
    $(SRCDIR)/audio_clip.c \
    $(SRCDIR)/clipregion.c \
    $(SRCDIR)/midiclip.c \
    $(SRCDIR)/track.c \
    $(SRCDIR)/undo.c \
    $(SRCDIR)/project.c \
    $(SRCDIR)/jackdaw-engine.c \
    $(SRCDIR)/pluginhost.c \
    $(SRCDIR)/pluginhost_lv2.c \
    $(SRCDIR)/pluginhost_clap.c \
    $(SRCDIR)/pluginhost_ladspa.c \
    $(SRCDIR)/lv2ui_bridge.c \
    $(SRCDIR)/knob.c \
    $(SRCDIR)/trackstrip.c \
    $(SRCDIR)/timeline.c \
    $(SRCDIR)/mixer.c \
    $(SRCDIR)/fxwindow.c \
    $(SRCDIR)/midiwindow.c \
    $(SRCDIR)/render.c \
    $(SRCDIR)/render_dialog.c \
    $(SRCDIR)/mainwindow.c

SRCS_CXX := \
    $(SRCDIR)/pluginhost_vst2.cpp

# Optional VST3 backend (heavier; needs the SDK). Enable with: make VST3=1
# NOTE: the exact SDK source list is version-sensitive — adjust VST3_SDK_SRC if
# your vst3sdk checkout differs.
VST3_SDK_OBJ :=
ifeq ($(VST3),1)
SRCS_CXX += $(SRCDIR)/pluginhost_vst3.cpp
VST3_SDK_SRC := \
    $(VST3SDK)/pluginterfaces/base/conststringtable.cpp \
    $(VST3SDK)/pluginterfaces/base/funknown.cpp \
    $(VST3SDK)/pluginterfaces/base/ustring.cpp \
    $(VST3SDK)/pluginterfaces/base/coreiids.cpp \
    $(VST3SDK)/public.sdk/source/vst/vstinitiids.cpp \
    $(VST3SDK)/public.sdk/source/common/commoniids.cpp \
    $(VST3SDK)/base/source/fobject.cpp \
    $(VST3SDK)/base/source/fstring.cpp \
    $(VST3SDK)/base/source/fbuffer.cpp \
    $(VST3SDK)/base/source/fdebug.cpp \
    $(VST3SDK)/base/source/updatehandler.cpp \
    $(VST3SDK)/base/source/baseiids.cpp \
    $(VST3SDK)/base/thread/source/flock.cpp \
    $(VST3SDK)/public.sdk/source/common/commonstringconvert.cpp \
    $(VST3SDK)/public.sdk/source/common/memorystream.cpp \
    $(VST3SDK)/public.sdk/source/common/threadchecker_linux.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/module.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/module_linux.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/hostclasses.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/plugprovider.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/processdata.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/parameterchanges.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/pluginterfacesupport.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/connectionproxy.cpp \
    $(VST3SDK)/public.sdk/source/vst/hosting/eventlist.cpp \
    $(VST3SDK)/public.sdk/source/vst/utility/stringconvert.cpp
VST3_SDK_OBJ := $(VST3_SDK_SRC:.cpp=.o)
endif

OBJS := $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o) $(VST3_SDK_OBJ)
DEPS := $(SRCS_C:.c=.d) $(SRCS_CXX:.cpp=.d)

# ---------------------------------------------------------------------------
# Out-of-process LV2 UI helpers — only for toolkits that can't run in our GTK3
# process (GtkUI/GTK2, Qt). X11/Gtk3 UIs are hosted in-process. Each helper is
# built only if its toolkit + suil are present; otherwise that UI type falls
# back to the generic parameter panel.
# ---------------------------------------------------------------------------
HELPERS :=
HAS_GTK2 := $(shell pkg-config --exists gtk+-2.0 gtk+-x11-2.0 2>/dev/null && echo 1)
HAS_QT5  := $(shell pkg-config --exists Qt5Widgets Qt5X11Extras 2>/dev/null && echo 1)
HAS_QT6  := $(shell pkg-config --exists Qt6Widgets 2>/dev/null && echo 1)

ifneq ($(HAS_LILV),)
ifneq ($(HAS_SUIL),)
# X11 helper: hosts X11UI editors out-of-process in a PURE-Xlib process (NO GTK,
# NO pango). Many X11 UIs draw with cairo's toy-font API; in any GTK/pango host
# that races libcairo's shared font cache -> use-after-free inside libcairo
# (gxtuner). Isolating the plugin as the sole cairo consumer fixes it — exactly
# how Reaper (which has no GTK/pango) stays immune. suil instantiates the X11UI
# natively (container == ui type == X11UI), so no wrapper/GTK is ever loaded.
HELPER_X11    := $(SRCDIR)/jackdaw-lv2ui-x11
HELPERS       += $(HELPER_X11)
H_X11_CFLAGS  := -g -O2 $(WARN) -I$(SRCDIR) -I$(EXTDIR) \
    $(shell pkg-config --cflags glib-2.0 x11 lilv-0 suil-0) -std=gnu99
H_X11_LIBS    := $(shell pkg-config --libs glib-2.0 x11 lilv-0 suil-0) -lm

$(HELPER_X11): $(SRCDIR)/lv2ui_x11_helper.c $(SRCDIR)/lv2ui_ipc.h
	$(CC) $(H_X11_CFLAGS) $< $(H_X11_LIBS) -o $@
	@echo "Built: $@"

ifneq ($(HAS_GTK2),)
HELPER_GTK2   := $(SRCDIR)/jackdaw-lv2ui-gtk2
HELPERS       += $(HELPER_GTK2)
# (lv2ui_helper.c defaults HELPER_CONTAINER_URI to GtkUI, which is the gtk2 type.)
H_GTK2_CFLAGS := -g -O2 $(WARN) -I$(SRCDIR) -I$(EXTDIR) \
    $(shell pkg-config --cflags gtk+-2.0 gtk+-x11-2.0 lilv-0 suil-0) -std=gnu99
H_GTK2_LIBS   := $(shell pkg-config --libs gtk+-2.0 gtk+-x11-2.0 lilv-0 suil-0) -lm

$(HELPER_GTK2): $(SRCDIR)/lv2ui_helper.c $(SRCDIR)/lv2ui_ipc.h
	$(CC) $(H_GTK2_CFLAGS) $< $(H_GTK2_LIBS) -o $@
	@echo "Built: $@"
endif
endif
endif

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

.PHONY: all clean

all: $(TARGET) $(HELPERS)

$(TARGET): $(OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@
	@echo "Built: $@"

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Generic rule for out-of-tree (VST3 SDK) C++ sources.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d $(TARGET) $(HELPERS)
