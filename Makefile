# Project Name
TARGET = project006

# Sources
CPP_SOURCES = src/main.cpp \
src/controls.cpp \
src/audio_engine.cpp \
src/tuner.cpp \
src/telemetry.cpp \
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
