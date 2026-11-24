// trackbench: runs a tracker backend + SkinSegmenter outside OBS over raw BGRA frames, frame by frame
// as the tracker does, and writes the raw meshes / masks plus timing and CPU per frame. Build one
// binary per backend and compare the dumps (Core ML vs ONNX Runtime A/B):
//   ffmpeg -i ~/Movies/dewy-demo/clip_pingpong.mp4 -t 8 -pix_fmt bgra -f rawvideo frames.bgra
//   R=~/Development/obs-beauty; T=$R/src/tracker
//   clang++ -std=c++17 -O2 -fobjc-arc -DUSE_COREML -I$R/src -I$T trackbench.cpp $T/mediapipe.cpp $T/tracker-util.cpp \
//     -x objective-c++ $T/backend-coreml.mm $T/coreml-common.mm $T/segmenter-coreml.mm \
//     -framework CoreML -framework CoreVideo -framework Accelerate -framework Foundation -o tb_coreml
//   O=$R/.deps/onnxruntime-osx-arm64-1.31.1
//   clang++ -std=c++17 -O2 -I$R/src -I$T -I$O/include trackbench.cpp $T/{mediapipe,tracker-util,backend-onnx,onnx-common,segmenter}.cpp -o tb_ort
//   ./tb_coreml frames.bgra 640 360 $R/build/coreml cm
//   ./tb_ort frames.bgra 640 360 $R/data/models ort $O/lib/libonnxruntime.1.31.1.dylib
// Output: <prefix>.mesh (per frame: int32 count + 468 x (x, y) float32 frame px), <prefix>.mask
// (per frame 256x256 RGBA8).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <sys/resource.h>
#include "backend.hpp"
#include "segmenter.hpp"
extern "C" void face_tracker_set_runtime_library(const char *);
static double cpu_ms() { struct rusage r; getrusage(RUSAGE_SELF, &r);
	return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3; }
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
int main(int argc, char **argv) {
	int w = atoi(argv[2]), h = atoi(argv[3]);
	if (argc > 6) face_tracker_set_runtime_library(argv[6]);
	FILE *f = fopen(argv[1], "rb");
	std::vector<uint8_t> all; { fseek(f, 0, SEEK_END); all.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(all.data(), 1, all.size(), f); fclose(f); }
	size_t fs = (size_t)w * h * 4; int n = (int)(all.size() / fs);
#ifdef USE_COREML
	auto be = beauty::create_coreml_backend(argv[4]);
#else
	auto be = beauty::create_onnx_backend(argv[4]);
#endif
	beauty::SkinSegmenter seg(argv[4]);
	std::string pre = argv[5];
	FILE *fm = fopen((pre + ".mesh").c_str(), "wb"), *fk = fopen((pre + ".mask").c_str(), "wb");
	// warm-up
	{ ft_face fc; bool full; be->detect(all.data(), w, h, w * 4, fc, full); std::vector<uint8_t> m; int mw, mh; seg.run(all.data(), w, h, w * 4, m, mw, mh); }
	be = nullptr;
#ifdef USE_COREML
	be = beauty::create_coreml_backend(argv[4]);
#else
	be = beauty::create_onnx_backend(argv[4]);
#endif
	std::vector<double> tl, ts; double cl = 0, cs = 0; int lost = 0, fulls = 0;
	for (int i = 0; i < n; i++) {
		const uint8_t *fr = all.data() + i * fs;
		ft_face fc = {}; bool full = false;
		double c0 = cpu_ms(); auto a = clk::now();
		bool ok = be->detect(fr, w, h, w * 4, fc, full);
		auto b = clk::now(); cl += cpu_ms() - c0; tl.push_back(ms(a, b));
		if (!ok) lost++; if (full) fulls++;
		int cnt = ok ? fc.region_count[FT_MESH] : 0;
		fwrite(&cnt, 4, 1, fm);
		std::vector<float> pts(468 * 2, 0.f);
		for (int k = 0; k < cnt && k < 468; k++) { pts[k * 2] = fc.pts[fc.region_start[FT_MESH] + k].x; pts[k * 2 + 1] = fc.pts[fc.region_start[FT_MESH] + k].y; }
		fwrite(pts.data(), 4, pts.size(), fm);
		std::vector<uint8_t> m; int mw = 0, mh = 0;
		c0 = cpu_ms(); a = clk::now();
		bool sok = seg.run(fr, w, h, w * 4, m, mw, mh);
		b = clk::now(); cs += cpu_ms() - c0; ts.push_back(ms(a, b));
		if (sok) fwrite(m.data(), 1, m.size(), fk);
	}
	auto stat = [](std::vector<double> v, double cpu, const char *nm) { std::sort(v.begin(), v.end()); double s = 0; for (double x : v) s += x;
		printf("  %-12s mean %5.2f  med %5.2f  p95 %5.2f ms   CPU %5.2f ms/frame\n", nm, s / v.size(), v[v.size() / 2], v[v.size() * 95 / 100], cpu / v.size()); };
	printf("%s: %d frames, lost %d, full detections %d\n", be->name(), n, lost, fulls);
	stat(tl, cl, "landmarks"); stat(ts, cs, "segmentation");
	fclose(fm); fclose(fk);
}
