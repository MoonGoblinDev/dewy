// Draws the lipstick mask (outer lips minus mouth opening) over a photo:
//   lipmask in.png out.png
#include <stdio.h>
#include <stdlib.h>
#include "face-tracker.h"
#include "face-geometry.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
static void path(CGContextRef c, const float *xy, int n, float H) {
  CGContextMoveToPoint(c, xy[0], H - xy[1]);
  for (int i = 1; i < n; i++) CGContextAddLineToPoint(c, xy[i * 2], H - xy[i * 2 + 1]);
  CGContextClosePath(c);
}
int main(int argc, char **argv) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], strlen(argv[1]), false);
  CGImageRef img = CGImageSourceCreateImageAtIndex(CGImageSourceCreateWithURL(url, NULL), 0, NULL);
  int W = (int)CGImageGetWidth(img), H = (int)CGImageGetHeight(img);
  int w = 640, h = (int)(640.0 * H / W) & ~1; uint8_t *buf = calloc(w * h, 4);
  CGContextRef sc = CGBitmapContextCreate(buf, w, h, 8, w * 4, CGColorSpaceCreateDeviceRGB(), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGContextDrawImage(sc, CGRectMake(0, 0, w, h), img);
  face_tracker_t *ft = face_tracker_create(getenv("BEAUTY_MODELS"));
  face_tracker_submit_bgra(ft, buf, w * 4, w, h, 1, 1);
  face_tracker_wait(ft, 1, 1000);
  ft_face f;
  if (!face_tracker_get_at(ft, 1, &f)) { printf("no face\n"); return 1; }
  float outer[400], open[400];
  int no = face_lips_outer(&f, W, H, outer, 200), nm = face_mouth_opening(&f, W, H, open, 200);
  CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, W * 4, CGColorSpaceCreateDeviceRGB(), (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CGContextDrawImage(c, CGRectMake(0, 0, W, H), img);
  CGContextSetRGBFillColor(c, 1, 0, 1, 0.45);
  path(c, outer, no, H);
  if (nm) path(c, open, nm, H);
  CGContextEOFillPath(c);  // even-odd: outer minus opening
  CGContextSetRGBStrokeColor(c, 0, 1, 1, 1); CGContextSetLineWidth(c, 1.5);
  if (nm) { path(c, open, nm, H); CGContextStrokePath(c); }
  CGImageRef out = CGBitmapContextCreateImage(c);
  CFURLRef ou = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[2], strlen(argv[2]), false);
  CGImageDestinationRef d = CGImageDestinationCreateWithURL(ou, CFSTR("public.png"), 1, NULL);
  CGImageDestinationAddImage(d, out, NULL); CGImageDestinationFinalize(d);
  printf("outer %d pts, opening %d pts\n", no, nm);
}
