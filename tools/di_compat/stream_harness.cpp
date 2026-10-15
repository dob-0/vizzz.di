// Writes a frame sequence and the events the node's Streamer sends for it
// (include/vizzz_stream.h), one JSON line per frame: {"frame":[512],"master":N,"event":<json or null>}.
// tools/di_compat/stream_side.js feeds the same frames to di's createDmxStream and compares.
#include <stdio.h>
#include <stdint.h>
#include "vizzz_core.h"

namespace st = vizzz::desk::stream;

int main() {
  st::Streamer s;
  s.reset();
  static char buf[st::kBufSize];
  uint8_t f[st::kSlots] = {0};
  uint32_t rng = 20261015u, now = 1000;
  for (int step = 0; step < 60; step++) {
    const int edits = (step % 11 == 0) ? 350 : (step % 5 == 0 ? 0 : 1 + step % 7);
    for (int e = 0; e < edits; e++) {
      rng = rng * 1664525u + 1013904223u;
      f[(rng >> 8) % st::kSlots] = uint8_t(rng >> 24);
    }
    const uint8_t master = uint8_t(step < 30 ? 255 : 180);   // the meta changes once
    now += st::kPeriodMs;
    const size_t n = s.step(now, f, st::Meta{master, false}, false, buf, sizeof(buf));
    printf("{\"frame\":[");
    for (int i = 0; i < st::kSlots; i++) printf(i ? ",%u" : "%u", f[i]);
    printf("],\"master\":%u,\"event\":%s}\n", master, n ? buf : "null");
  }
  return 0;
}
