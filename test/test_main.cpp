#include <cstdio>

#include "check.hpp"

int g_checkFailures = 0;

int main() {
  struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"pixel", testPixel},
      {"graphics2d", testGraphics2D},
      {"gfx3d", testGfx3D},
      {"tools", testTools},
  };
  for (auto &t : tests) {
    int before = g_checkFailures;
    std::printf("[%s]\n", t.name);
    t.fn();
    std::printf("  %s\n", g_checkFailures == before ? "ok" : "FAILED");
  }
  std::printf("%d failure(s)\n", g_checkFailures);
  return g_checkFailures ? 1 : 0;
}
