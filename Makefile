# JackDAW — plain Makefile (no autotools)
# Usage:  make [-j$(nproc)]   build
#         make clean           remove build artifacts
#         make VST3=1          also build the VST3 backend (needs the SDK)
#
SRCDIR  := src
EXTDIR  := ext
TARGET  := $(SRCDIR)/jackdaw

CC      := gcc
CXX     := g++

VST3SDK ?= /home/human/third_party/vst3sdk

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

# VST2 and CLAP backends use vendored headers in ext/ — always available.
OPT_DEFS += -DHAVE_VST2=1 -DHAVE_CLAP=1

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS_REQ) $(PKGS_OPT))
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS_REQ) $(PKGS_OPT))

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
COMMON := -g -O2 $(WARN) -I$(SRCDIR) -I$(EXTDIR) $(PKG_CFLAGS) $(OPT_DEFS)

CFLAGS   := $(COMMON) -std=gnu99
CXXFLAGS := $(COMMON) -std=c++17

LDFLAGS := \
    $(PKG_LIBS) \
    -lm -lpthread -ldl -lstdc++

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRCS_C := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/settings.c \
    $(SRCDIR)/um.c \
    $(SRCDIR)/audio_clip.c \
    $(SRCDIR)/clipregion.c \
    $(SRCDIR)/track.c \
    $(SRCDIR)/project.c \
    $(SRCDIR)/jackdaw-engine.c \
    $(SRCDIR)/pluginhost.c \
    $(SRCDIR)/pluginhost_lv2.c \
    $(SRCDIR)/pluginhost_clap.c \
    $(SRCDIR)/knob.c \
    $(SRCDIR)/trackstrip.c \
    $(SRCDIR)/timeline.c \
    $(SRCDIR)/mixer.c \
    $(SRCDIR)/fxwindow.c \
    $(SRCDIR)/mainwindow.c

SRCS_CXX := \
    $(SRCDIR)/pluginhost_vst2.cpp

# Optional VST3 backend (heavier; needs the SDK). Enable with: make VST3=1
# NOTE: the exact SDK source list is version-sensitive — adjust VST3_SDK_SRC if
# your vst3sdk checkout differs.
VST3_SDK_OBJ :=
ifeq ($(VST3),1)
OPT_DEFS += -DHAVE_VST3=1
CXXFLAGS += -I$(VST3SDK) -DRELEASE=1
SRCS_CXX += $(SRCDIR)/pluginhost_vst3.cpp
VST3_SDK_SRC := \
    $(VST3SDK)/pluginterfaces/base/conststringtable.cpp \
    $(VST3SDK)/pluginterfaces/base/funknown.cpp \
    $(VST3SDK)/pluginterfaces/base/ustring.cpp \
    $(VST3SDK)/pluginterfaces/base/coreiids.cpp \
    $(VST3SDK)/public.sdk/source/vst/vstinitiids.cpp \
    $(VST3SDK)/base/source/fobject.cpp \
    $(VST3SDK)/base/source/fstring.cpp \
    $(VST3SDK)/base/source/fbuffer.cpp \
    $(VST3SDK)/base/source/fdebug.cpp \
    $(VST3SDK)/base/source/updatehandler.cpp \
    $(VST3SDK)/base/source/baseiids.cpp \
    $(VST3SDK)/base/thread/source/flock.cpp \
    $(VST3SDK)/public.sdk/source/common/commonstringconvert.cpp \
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
# Rules
# ---------------------------------------------------------------------------

.PHONY: all clean

all: $(TARGET)

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
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d $(TARGET)
