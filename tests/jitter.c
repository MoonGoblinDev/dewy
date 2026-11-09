// Static face + per-frame sensor noise -> RMS frame-to-frame landmark motion in 1080p pixels.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "face-tracker.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
int main(int argc, char **argv) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], strlen(argv[1]), false);
  CGImageRef img = CGImageSourceCreateImageAtIndex(CGImageSourceCreateWithURL(url, NULL), 0, NULL);
  int w = 640, h = 360; uint8_t *base = calloc(w * h, 4), *buf = calloc(w * h, 4);
  CGContextRef ctx = CGBitmapContextCreate(base, w, h, 8, w * 4, CGColorSpaceCreateDeviceRGB(), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  face_tracker_t *ft = face_tracker_create(getenv("BEAUTY_MODELS"));
  ft_face prev = {0}; double sum = 0; int n = 0; srand(1);
  for (uint64_t i = 1; i <= 240; i++) {
    for (int k = 0; k < w * h * 4; k++) { int v = base[k] + (rand() % 9) - 4; buf[k] = v < 0 ? 0 : v > 255 ? 255 : v; }
    face_tracker_submit_bgra(ft, buf, w * 4, w, h, i, i * 16666667ULL);
    face_tracker_wait(ft, i, 100);
    ft_face f; face_tracker_get_at(ft, i, &f);
    if (i > 60 && prev.valid && f.num_points == prev.num_points) {
      for (int p = 0; p < f.num_points; p++) { double dx = (f.pts[p].x - prev.pts[p].x) * 1920, dy = (f.pts[p].y - prev.pts[p].y) * 1080; sum += dx*dx + dy*dy; n++; }
    }
    prev = f;
  }
  float ms, det; face_tracker_stats(ft, &ms, &det);
  printf("jitter %.3f px RMS/frame (1080p), %.2f ms, detections %.0f%%\n", sqrt(sum / n), ms, det * 100);
}
