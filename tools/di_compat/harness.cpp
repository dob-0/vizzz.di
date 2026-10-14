// Feeds packets built by di.iiii's own serverXR/src/lighting code through the
// firmware's parsers (include/vizzz_core.h), and writes the firmware's
// ArtPollReply for di's parser. Driven by check_di_compat.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vizzz_core.h"

static size_t slurp(const char* path, uint8_t* buf, size_t cap) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  size_t n = fread(buf, 1, cap, f);
  fclose(f);
  return n;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: harness poll|artdmx|sacn|reply FILE\n"); return 2; }
  static uint8_t b[2048];
  int fails = 0;
  if (!strcmp(argv[1], "poll")) {
    size_t n = slurp(argv[2], b, sizeof(b));
    bool ok = vizzz::artnet::isArtPoll(b, n);
    printf("ArtPoll from di (%zu bytes): %s\n", n, ok ? "detected" : "NOT detected");
    fails += !ok;
  } else if (!strcmp(argv[1], "artdmx")) {
    size_t n = slurp(argv[2], b, sizeof(b));
    vizzz::artnet::DmxView v{};
    bool ok = vizzz::artnet::parseDmx(b, n, v);
    printf("ArtDmx from di: %s port-address=%u seq=%u len=%u ch1=%u\n", ok ? "parsed" : "REJECTED",
           v.portAddress, v.sequence, v.length, ok ? v.data[0] : 0);
    fails += !(ok && v.portAddress == 0 && v.data[0] == 11);
  } else if (!strcmp(argv[1], "sacn")) {
    size_t n = slurp(argv[2], b, sizeof(b));
    vizzz::sacn::DataView v{};
    bool ok = vizzz::sacn::parseData(b, n, v);
    printf("sACN from di: %s universe=%u (node listens on %u) prio=%u len=%u ch1=%u\n",
           ok ? "parsed" : "REJECTED", v.universe, vizzz::sacn::universeForPortAddress(0),
           v.priority, v.length, ok ? v.data[0] : 0);
    fails += !(ok && v.universe == vizzz::sacn::universeForPortAddress(0) && v.data[0] == 22);
  } else if (!strcmp(argv[1], "reply")) {
    vizzz::artnet::PollReplyInfo in{};
    const uint8_t ip[4] = {127, 0, 0, 1}, mac[6] = {0x24, 0x6F, 0x28, 1, 2, 3};
    memcpy(in.ip, ip, 4); memcpy(in.mac, mac, 6);
    in.net = 0; in.subnet = 0; in.uni = 3;
    in.shortName = "vizzz-test"; in.longName = "vizzz.di vizzz-test"; in.report = "ok";
    in.outputActive = true; in.refreshHz = 43;
    size_t n = vizzz::artnet::buildPollReply(b, in);
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(b, 1, n, f) != n) return 2;
    fclose(f);
    printf("ArtPollReply written: %zu bytes\n", n);
  }
  return fails ? 1 : 0;
}
