# WASM build of the demo2d sample (requires Emscripten).
# Output goes to docs/example/demo2d/ so that it can be served as a static page.

EMCC ?= emcc
CXX ?= g++

REPO_ROOT := ../../..
OUT_DIR := $(REPO_ROOT)/docs/example/demo2d
OUT := $(OUT_DIR)/demo2d.wasm

SRCS := main.cpp scene.cpp $(wildcard $(REPO_ROOT)/src/gfx2d/*.cpp) $(wildcard $(REPO_ROOT)/src/gfx3d/*.cpp)
HDRS := $(wildcard $(REPO_ROOT)/include/shapoco/gfx2d/*.hpp) \
        $(wildcard $(REPO_ROOT)/include/shapoco/gfx3d/*.hpp) \
        scene.hpp

# Perspective correction level (0 = off, 1 = vertical only, 2 = full).
# Leave empty to use the library default (1).
SHAPOGFX3D_CORRECT_PERSPECTIVE ?=

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I $(REPO_ROOT)/include -I . \
            $(if $(SHAPOGFX3D_CORRECT_PERSPECTIVE),-DSHAPOGFX3D_CORRECT_PERSPECTIVE=$(SHAPOGFX3D_CORRECT_PERSPECTIVE))

EXPORTS := _demo2d_init,_demo2d_frame,_demo2d_get_fb,_demo2d_get_width,_demo2d_get_height

.PHONY: all clean serve native

all: $(OUT)

$(OUT): $(SRCS) $(HDRS) Makefile
	$(EMCC) $(CXXFLAGS) $(SRCS) --no-entry -sSTANDALONE_WASM=1 \
		-sEXPORTED_FUNCTIONS=$(EXPORTS) \
		-o $@

# Native build: renders one frame to demo2d.ppm
native: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o demo2d

clean:
	rm -f $(OUT) demo2d demo2d.ppm

# Serve docs/ over HTTP and open http://localhost:8000/example/demo2d/
# (or use ./launch_web_server.sh in the repository root)
# (fetch() does not work from file://)
serve:
	python3 -m http.server -d $(REPO_ROOT)/docs 8000
