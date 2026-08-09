# Project Name
TARGET = project006

# Optional matrix-build overrides. Normal use changes only DIAG_STAGE in
# src/diagnostic_config.h; these variables allow reproducible command-line builds.
ifneq ($(DIAG_STAGE),)
C_DEFS += -DDIAG_STAGE=$(DIAG_STAGE)
endif
ifneq ($(DIAG_AUDIO_MODE),)
C_DEFS += -DDIAG_AUDIO_MODE=$(DIAG_AUDIO_MODE)
endif
ifneq ($(DIAG_FX_CHORUS),)
C_DEFS += -DDIAG_FX_CHORUS=$(DIAG_FX_CHORUS)
endif
ifneq ($(DIAG_FX_REVERB),)
C_DEFS += -DDIAG_FX_REVERB=$(DIAG_FX_REVERB)
endif
ifneq ($(DIAG_FX_CRUSHER),)
C_DEFS += -DDIAG_FX_CRUSHER=$(DIAG_FX_CRUSHER)
endif
ifneq ($(DIAG_FX_GRANULAR),)
C_DEFS += -DDIAG_FX_GRANULAR=$(DIAG_FX_GRANULAR)
endif
ifneq ($(DIAG_FORCE_EFFECT),)
C_DEFS += -DDIAG_FORCE_EFFECT=$(DIAG_FORCE_EFFECT)
endif

# Sources
CPP_SOURCES = src/main.cpp \
src/controls.cpp \
src/audio_engine.cpp \
src/tuner.cpp \
src/telemetry.cpp \
src/thermal_monitor.cpp \
gml/GuitarPedal/Effect-Modules/base_effect_module.cpp \
gml/GuitarPedal/Effect-Modules/chorus_module.cpp \
gml/GuitarPedal/Effect-Modules/reverb_module.cpp \
gml/GuitarPedal/Effect-Modules/crusher_module.cpp \
gml/GuitarPedal/Effect-Modules/granulardelay_module.cpp \
gml/GuitarPedal/Util/audio_utilities.cpp \
gml/GuitarPedal/Util/granularplayermod.cpp \
$(wildcard gml/GuitarPedal/Effect-Modules/Chopper/*.cpp) \
$(wildcard gml/GuitarPedal/Effect-Modules/Delays/*.cpp)

CPP_SOURCES := $(filter-out gml/GuitarPedal/main.cpp,$(CPP_SOURCES))

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR  = ../../DaisySP

# Add DaisySP-LGPL (for ReverbSc / Tone, etc)
DAISYSP_LGPL_DIR = ../../DaisySP/DaisySP-LGPL
C_INCLUDES += -I$(DAISYSP_LGPL_DIR)/Source
C_INCLUDES += -I$(DAISYSP_LGPL_DIR)/Source/Effects
C_INCLUDES += -I$(DAISYSP_LGPL_DIR)/Source/Filters
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Utility
CPP_SOURCES += $(DAISYSP_LGPL_DIR)/Source/Effects/reverbsc.cpp
CPP_SOURCES += $(DAISYSP_LGPL_DIR)/Source/Filters/tone.cpp

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

C_INCLUDES += -Igml/GuitarPedal
C_INCLUDES += -Isrc
C_INCLUDES += -Igml/GuitarPedal/Effect-Modules
C_INCLUDES += -Igml/GuitarPedal/Effect-Modules/Chopper
C_INCLUDES += -Igml/GuitarPedal/Effect-Modules/Delays
C_INCLUDES += -Igml/GuitarPedal/Util

include $(SYSTEM_FILES_DIR)/Makefile
