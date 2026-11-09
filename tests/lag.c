// Face pans at `speed` analysis px/frame after a still period; reports how far (1080p px)
// the smoothed landmark centroid trails the true position, plus still-period jitter.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "face-tracker.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
static float cx(const ft_face *f) { float s = 0; for (int i = 0; i < f->num_points; i++) s += f->pts[i].x; return s / f->num_points; }
int main(int argc, char **argv) {
  float speed = atof(argv[2]);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], strlen(argv[1]), false);
  CGImageRef img = CGImageSourceCreateImageAtIndex(CGImageSourceCreateWithURL(url, NULL), 0, NULL);
  int w = 640, h = 360; uint8_t *base = calloc(w * h, 4), *buf = calloc(w * h, 4);
  CGContextRef ctx = CGBitmapContextCreate(base, w, h, 8, w * 4, CGColorSpaceCreateDeviceRGB(), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  face_tracker_t *ft = face_tracker_create(getenv("BEAUTY_MODELS")); srand(1);
  float ref = 0, maxlag = 0, sumlag = 0; int nl = 0;
  for (uint64_t i = 1; i <= 150; i++) {
    int shift = i > 90 ? (int)lroundf((i - 90) * speed) : 0;  // pan left->right for 60 frames
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      int sx = x - shift; if (sx < 0) sx = 0;
      for (int c = 0; c < 4; c++) { int v = base[(y * w + sx) * 4 + c] + (rand() % 9) - 4; buf[(y * w + x) * 4 + c] = v < 0 ? 0 : v > 255 ? 255 : v; }
    }
    face_tracker_submit_bgra(ft, buf, w * 4, w, h, i, i * 16666667ULL);
    face_tracker_wait(ft, i, 100);
    ft_face f; face_tracker_get_at(ft, i, &f);
    if (i == 90) ref = cx(&f);
    if (i > 100) { float lag = ((ref + shift / (float)w) - cx(&f)) * 1920; sumlag += lag; nl++; if (fabsf(lag) > maxlag) maxlag = fabsf(lag); }
  }
  printf("speed %.1f px/frame (%.0f px/s at 1080p): mean lag %.1f px, max %.1f px\n", speed, speed * 3 * 60, sumlag / nl, maxlag);
}
