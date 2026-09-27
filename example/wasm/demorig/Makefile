# WASM build of the demorig sample (requires Emscripten).
# Output goes to docs/example/demorig/ so that it can be served as a static page.

EMCC ?= emcc
CXX ?= g++
PYTHON ?= python3

REPO_ROOT := ../../..
OUT_DIR := $(REPO_ROOT)/docs/example/demorig
OUT := $(OUT_DIR)/demorig.wasm

COMMON := $(REPO_ROOT)/example/common/demorig

SRCS := main.cpp $(COMMON)/demorig.cpp $(COMMON)/scene.cpp \
        $(wildcard $(REPO_ROOT)/src/gfx2d/*.cpp)
HDRS := $(wildcard $(REPO_ROOT)/include/shapoco/gfx2d/*.hpp) \
        $(COMMON)/demorig.hpp $(COMMON)/scene.hpp $(COMMON)/model/rgb_chan.hpp

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I $(REPO_ROOT)/include -I $(COMMON)

EXPORTS := _demorig_init,_demorig_frame,_demorig_get_fb,_demorig_get_width,_demorig_get_height
EXPORTS := $(EXPORTS),_demorig_set_screen,_demorig_pointer_down,_demorig_pointer_move,_demorig_pointer_up

# The character (checked in; `make model` regenerates it from the assets):
# one atlas for this build (a power-of-two stride, which the RP2 interpolator
# path wants), and one texture per image for the M5Stack builds, whose parts
# come from flash through a cache and are then stored without row padding
# (a third fewer cache lines per frame)
ASSETS := $(REPO_ROOT)/assets/2d/rgb_chan
DBONES := $(PYTHON) $(REPO_ROOT)/bin/dbones2cpp --scale 0.8 --fit-rotate --out-format auto
INPUTS := $(ASSETS)/rgb_chan_ske.json $(ASSETS)/Armature_animtion0.dbani

.PHONY: all clean serve native model

all: $(OUT)

$(OUT): $(SRCS) $(HDRS) Makefile
	$(EMCC) $(CXXFLAGS) $(SRCS) --no-entry -sSTANDALONE_WASM=1 \
		-sEXPORTED_FUNCTIONS=$(EXPORTS) \
		-o $@

model:
	$(DBONES) $(INPUTS) $(COMMON)/model/rgb_chan.hpp
	$(DBONES) --atlas-width 0 $(INPUTS) $(COMMON)/model/rgb_chan_sep.hpp

# Native build: renders one frame to demorig.ppm
native: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o demorig

clean:
	rm -f $(OUT) demorig demorig.ppm

# Serve docs/ over HTTP and open http://localhost:8000/example/demorig/
# (or use ./launch_web_server.sh in the repository root)
# (fetch() does not work from file://)
serve:
	python3 -m http.server -d $(REPO_ROOT)/docs 8000
