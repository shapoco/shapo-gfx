# WASM build of the demorig sample (requires Emscripten).
# Output goes to docs/example/demorig/ so that it can be served as a static page.

EMCC ?= emcc
CXX ?= g++
PYTHON ?= python3

REPO_ROOT := ../../..
OUT_DIR := $(REPO_ROOT)/docs/example/demorig
OUT := $(OUT_DIR)/demorig.wasm

SRCS := main.cpp scene.cpp $(wildcard $(REPO_ROOT)/src/gfx2d/*.cpp)
HDRS := $(wildcard $(REPO_ROOT)/include/shapoco/gfx2d/*.hpp) \
        scene.hpp model/rgb_chan.hpp

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I $(REPO_ROOT)/include -I .

EXPORTS := _demorig_init,_demorig_frame,_demorig_get_fb,_demorig_get_width,_demorig_get_height

# The character (checked in; `make model` regenerates it from the assets)
ASSETS := $(REPO_ROOT)/assets/2d/rgb_chan

.PHONY: all clean serve native model

all: $(OUT)

$(OUT): $(SRCS) $(HDRS) Makefile
	$(EMCC) $(CXXFLAGS) $(SRCS) --no-entry -sSTANDALONE_WASM=1 \
		-sEXPORTED_FUNCTIONS=$(EXPORTS) \
		-o $@

model:
	$(PYTHON) $(REPO_ROOT)/bin/dbones2cpp --scale 0.5 \
		$(ASSETS)/rgb_chan_ske.json $(ASSETS)/Armature_animtion0.dbani model/rgb_chan.hpp

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
