// widget_pty_harness — host-side loopback for the USB widget input, no board needed.
// Opens a pseudo-terminal, prints its slave path, and runs the firmware's own
// parser (include/vizzz_core.h vizzz::widget) on every byte written to it, the
// way pollUsb() does on UART0: label 6 applied to a 512-channel array, labels 3
// and 10 answered, log text interleaved on the "UART" every 200 ms. di.iiii's
// enttec.js (or tools/usb_probe.js params) is then pointed at the slave path.
//   g++ -std=c++17 -Iinclude tools/widget_pty_harness.cpp -o <scratch>/wph
//   <scratch>/wph <seconds> <ch>     -> prints "pty /dev/pts/N", then a summary
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "vizzz_core.h"

static int fd;
static uint8_t vals[512];
static unsigned dmxFrames, otherFrames;

static long ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void onFrame(uint8_t label, const uint8_t* d, uint16_t len) {
  static uint8_t reply[600];
  if (label == vizzz::widget::kLabelSendDmx) { if (vizzz::widget::applyDmx(d, len, vals)) dmxFrames++; return; }
  otherFrames++;
  size_t n = 0;
  if (label == vizzz::widget::kLabelGetParams)
    n = vizzz::widget::buildParamsReply(reply, sizeof(reply), {0x0100, 176, 12, 43}, vizzz::widget::paramsRequestSize(d, len));
  if (label == vizzz::widget::kLabelGetSerial) n = vizzz::widget::buildSerialReply(reply, sizeof(reply), 1234567);
  if (n && write(fd, reply, n) != ssize_t(n)) perror("write");
}

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 5, ch = argc > 2 ? atoi(argv[2]) : 64;
  fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (fd < 0 || grantpt(fd) || unlockpt(fd)) { perror("pty"); return 1; }
  termios t; tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t);
  printf("pty %s\n", ptsname(fd)); fflush(stdout);
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  long end = ms() + seconds * 1000L, nextLog = 0;
  uint8_t buf[256];
  while (ms() < end) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) vizzz::widget::feed(p, buf, size_t(n), onFrame);
    else usleep(2000);
    if (ms() >= nextLog) { nextLog = ms() + 200; const char log[] = "[log] node text on the same UART\r\n"; (void)!write(fd, log, sizeof(log) - 1); }
  }
  printf("dmx_frames %u other_frames %u parser_frames %u errors %u skipped %u ch%d %u\n", dmxFrames, otherFrames,
         unsigned(p.frames), unsigned(p.errors), unsigned(p.skipped), ch, vals[ch - 1]);
  return 0;
}
