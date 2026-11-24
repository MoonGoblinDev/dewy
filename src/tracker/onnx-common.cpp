#include "onnx-common.hpp"

#ifdef _WIN32
#include <dml_provider_factory.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_2.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>

#ifdef BEAUTY_HAVE_LIBOBS
#include <util/base.h>
#endif

static std::string g_ort_library;
extern "C" void face_tracker_set_runtime_library(const char *path)
{
	g_ort_library = path ? path : "";
}

/* DirectML adapter: index into IDXGIFactory1::EnumAdapters1, which is what the DML provider's
 * device_id means. -1 = not known yet (use 0). */
static std::atomic<int> g_dml_adapter{-1};
static std::string g_dml_adapter_name;

extern "C" void face_tracker_set_render_device(void *device)
{
#ifdef _WIN32
	if (!device || g_dml_adapter.load() >= 0)
		return;
	/* OBS's ID3D11Device -> its adapter's LUID -> the same adapter's DXGI index. OBS sorts
	 * adapters its own way (high-performance GPU first on hybrid laptops), so its adapter
	 * number can't be handed to DirectML as is: on such laptops index 0 is the iGPU. */
	IUnknown *unk = static_cast<IUnknown *>(device);
	IDXGIDevice *dxgi_dev = nullptr;
	if (FAILED(unk->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))))
		return;
	IDXGIAdapter *adapter = nullptr;
	DXGI_ADAPTER_DESC want = {};
	bool have = SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&want));
	if (adapter)
		adapter->Release();
	dxgi_dev->Release();
	if (!have)
		return;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return;
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d = {};
		bool match = SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.LowPart == want.AdapterLuid.LowPart &&
			     d.AdapterLuid.HighPart == want.AdapterLuid.HighPart;
		a->Release();
		if (match) {
			char name[128];
			WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
			g_dml_adapter_name = name;
			g_dml_adapter = (int)i;
			break;
		}
	}
	factory->Release();
#else
	(void)device;
#endif
}

namespace beauty {

void ort_log(const char *fmt, const std::string &arg)
{
	char msg[512];
	snprintf(msg, sizeof(msg), fmt, arg.c_str());
	tracker_log("%s", msg);
}

bool ort_load()
{
	static std::once_flag once;
	static bool ok = false;
	std::call_once(once, [] {
		std::string path = g_ort_library;
		if (const char *env = getenv("BEAUTY_ORT_LIB"))
			path = env;
		if (path.empty()) {
			ort_log("ONNX Runtime library path not set");
			return;
		}
		typedef const OrtApiBase *(*GetBaseFn)();
		GetBaseFn get_base = nullptr;
#ifdef _WIN32
		int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
		std::wstring wpath(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
		/* altered search path: DirectML.dll is found next to the runtime */
		HMODULE lib = LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (lib)
			get_base = (GetBaseFn)GetProcAddress(lib, "OrtGetApiBase");
#else
		void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (lib)
			get_base = (GetBaseFn)dlsym(lib, "OrtGetApiBase");
#endif
		if (!get_base) {
			ort_log("could not load ONNX Runtime from %s", path);
			return;
		}
		const OrtApi *api = get_base()->GetApi(ORT_API_VERSION);
		if (!api) {
			ort_log("ONNX Runtime at %s is too old for this build", path);
			return;
		}
		Ort::InitApi(api);
		ok = true;
	});
	return ok;
}

Ort::Env &ort_env()
{
	static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "dewy");
	return env;
}

static std::vector<char> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* ---- explicit Conv padding ----
 * Core ML's ML Program converter refuses a Conv without a `pads` attribute ("Required param 'pad'
 * is missing"), although ONNX defines a missing one as all zeros. MediaPipe's models leave it out on
 * many convolutions, which pushes Core ML into its older format, where the models split into many
 * pieces bouncing between Core ML and the CPU. Writing the zeros out changes nothing in the maths.
 * A minimal protobuf rewrite: ModelProto.graph(7) -> GraphProto.node(1) -> NodeProto.attribute(5). */
namespace pb {
static bool varint(const uint8_t *&p, const uint8_t *e, uint64_t &v)
{
	v = 0;
	for (int sh = 0; p < e && sh < 64; sh += 7) {
		uint8_t b = *p++;
		v |= (uint64_t)(b & 0x7f) << sh;
		if (!(b & 0x80))
			return true;
	}
	return false;
}
static void put_varint(std::string &o, uint64_t v)
{
	while (v >= 0x80) {
		o.push_back((char)(v | 0x80));
		v >>= 7;
	}
	o.push_back((char)v);
}
/* Calls fn(field, wire, start, end) for each field; returns false on malformed input. */
template<class F> static bool fields(const uint8_t *p, const uint8_t *e, F fn)
{
	while (p < e) {
		const uint8_t *start = p;
		uint64_t tag, len;
		if (!varint(p, e, tag))
			return false;
		int wire = (int)(tag & 7);
		if (wire == 0) {
			if (!varint(p, e, len))
				return false;
		} else if (wire == 1) {
			p += 8;
		} else if (wire == 5) {
			p += 4;
		} else if (wire == 2) {
			if (!varint(p, e, len) || len > (uint64_t)(e - p))
				return false;
			p += len;
		} else {
			return false;
		}
		if (p > e)
			return false;
		fn((int)(tag >> 3), wire, start, p);
	}
	return true;
}
/* The payload of a length-delimited field [start, end). */
static void payload(const uint8_t *start, const uint8_t *end, const uint8_t *&ps, const uint8_t *&pe)
{
	const uint8_t *p = start;
	uint64_t tag, len;
	varint(p, end, tag);
	varint(p, end, len);
	ps = p;
	pe = p + len;
}
static void put_len(std::string &o, int field, const std::string &body)
{
	put_varint(o, ((uint64_t)field << 3) | 2);
	put_varint(o, body.size());
	o += body;
}
static std::string fix_node(const uint8_t *s, const uint8_t *e, int &fixed)
{
	std::string op;
	bool has_pads = false;
	fields(s, e, [&](int f, int w, const uint8_t *a, const uint8_t *b) {
		const uint8_t *ps, *pe;
		if (f == 4 && w == 2) {
			payload(a, b, ps, pe);
			op.assign((const char *)ps, pe - ps);
		} else if (f == 5 && w == 2) {
			payload(a, b, ps, pe);
			fields(ps, pe, [&](int af, int aw, const uint8_t *x, const uint8_t *y) {
				const uint8_t *ns, *ne;
				if (af == 1 && aw == 2) {
					payload(x, y, ns, ne);
					if (std::string((const char *)ns, ne - ns) == "pads")
						has_pads = true;
				}
			});
		}
	});
	std::string out((const char *)s, e - s);
	if ((op == "Conv" || op == "ConvTranspose") && !has_pads) {
		std::string attr;
		put_len(attr, 1, "pads");
		for (int i = 0; i < 4; i++) { /* ints (field 8), unpacked: four zeros */
			put_varint(attr, 8 << 3);
			put_varint(attr, 0);
		}
		put_varint(attr, 20 << 3); /* type = INTS (7) */
		put_varint(attr, 7);
		put_len(out, 5, attr);
		fixed++;
	}
	return out;
}
} // namespace pb

static std::vector<char> explicit_conv_pads(const std::vector<char> &in, int &fixed)
{
	fixed = 0;
	const uint8_t *p = (const uint8_t *)in.data(), *e = p + in.size();
	std::string model;
	bool ok = pb::fields(p, e, [&](int f, int w, const uint8_t *a, const uint8_t *b) {
		if (f != 7 || w != 2) { /* everything but the graph: copied as is */
			model.append((const char *)a, b - a);
			return;
		}
		const uint8_t *gs, *ge;
		pb::payload(a, b, gs, ge);
		std::string graph;
		pb::fields(gs, ge, [&](int gf, int gw, const uint8_t *x, const uint8_t *y) {
			if (gf != 1 || gw != 2) {
				graph.append((const char *)x, y - x);
				return;
			}
			const uint8_t *ns, *ne;
			pb::payload(x, y, ns, ne);
			pb::put_len(graph, 1, pb::fix_node(ns, ne, fixed));
		});
		pb::put_len(model, 7, graph);
	});
	if (!ok || !fixed)
		return in;
	return std::vector<char>(model.begin(), model.end());
}

static OrtModelPtr load_model(const std::string &path, bool prefer_accel)
{
	std::vector<char> bytes = read_file(path);
#ifdef __APPLE__
	if (prefer_accel && !bytes.empty()) {
		int fixed = 0;
		bytes = explicit_conv_pads(bytes, fixed);
		(void)fixed;
	}
#endif
	if (bytes.empty()) {
		ort_log("missing model %s", path);
		return nullptr;
	}
	auto m = std::make_shared<OrtModel>();
	auto build = [&](bool accel, bool mlprogram) {
		(void)mlprogram;
		Ort::SessionOptions so;
		so.SetIntraOpNumThreads(2);
		/* ORT's workers busy-spin between runs by default: ~85% of a core wasted for a model
		 * that runs a few times a second. Sleep instead. */
		so.AddConfigEntry("session.intra_op.allow_spinning", "0");
		so.AddConfigEntry("session.inter_op.allow_spinning", "0");
		so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		bool on = false;
		if (accel) {
#ifdef _WIN32
			const OrtDmlApi *dml = nullptr;
			if (Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
								  reinterpret_cast<const void **>(&dml)) == nullptr &&
			    dml) {
				so.DisableMemPattern();
				so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
				int dev = g_dml_adapter.load();
				if (const char *env = getenv("DEWY_DML_ADAPTER")) /* dev override */
					dev = atoi(env);
				on = dml->SessionOptionsAppendExecutionProvider_DML(so, dev < 0 ? 0 : dev) == nullptr;
			}
#elif defined(__APPLE__)
			try {
				/* ML Program: takes the (explicitly padded) MediaPipe models nearly whole; the
				 * older NeuralNetwork format splits them into many Core ML / CPU pieces */
				so.AppendExecutionProvider("CoreML", {{"ModelFormat", mlprogram ? "MLProgram" : "NeuralNetwork"}});
				on = true;
			} catch (const Ort::Exception &) {
				on = false;
			}
#endif
		}
		m->session = Ort::Session(ort_env(), bytes.data(), bytes.size(), so);
		m->accelerated = on;
	};
	try {
		build(prefer_accel, true);
	} catch (const Ort::Exception &e) {
		if (!prefer_accel)
			throw;
#ifdef __APPLE__
		try { /* a model ML Program can't compile: the older Core ML format */
			build(true, false);
		} catch (const Ort::Exception &e2) {
			ort_log("accelerated provider failed (%s), using CPU", e2.what());
			build(false, false);
		}
#else
		ort_log("accelerated provider failed (%s), using CPU", e.what());
		build(false, false);
#endif
	}
	Ort::AllocatorWithDefaultOptions alloc;
	for (size_t i = 0; i < m->session.GetInputCount(); i++)
		m->in_names.push_back(m->session.GetInputNameAllocated(i, alloc).get());
	for (size_t i = 0; i < m->session.GetOutputCount(); i++)
		m->out_names.push_back(m->session.GetOutputNameAllocated(i, alloc).get());
	for (auto &s : m->in_names)
		m->in.push_back(s.c_str());
	for (auto &s : m->out_names)
		m->out.push_back(s.c_str());
	if (m->accelerated)
		ort_warm_up(*m);
	return m;
}

OrtModelPtr ort_model(const std::string &path, bool prefer_accel)
{
	if (!ort_load())
		return nullptr;
	if (const char *cpu = getenv("DEWY_ORT_CPU"); cpu && *cpu == '1')
		prefer_accel = false; /* dev knob: compare CPU against the GPU execution provider */
	/* one load at a time, so filters created together share the first one's sessions */
	static std::mutex load_m;
	static std::map<std::string, std::weak_ptr<const OrtModel>> cache;
	std::lock_guard<std::mutex> g(load_m);
	std::string key = path + (prefer_accel ? "|accel" : "|cpu");
	if (auto hit = cache[key].lock())
		return hit;
	OrtModelPtr m = load_model(path, prefer_accel);
	if (m)
		cache[key] = m;
	return m;
}

void ort_warm_up(const OrtModel &m)
{
	try {
		Ort::TypeInfo ti = m.session.GetInputTypeInfo(0);
		std::vector<int64_t> shape = ti.GetTensorTypeAndShapeInfo().GetShape();
		size_t n = 1;
		for (auto &d : shape) {
			if (d <= 0)
				d = 1;
			n *= (size_t)d;
		}
		std::vector<float> zeros(n, 0.0f);
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, zeros.data(), n, shape.data(), shape.size());
		m.run(t);
	} catch (const Ort::Exception &e) {
		ort_log("warm-up run failed: %s", e.what());
	}
}

const char *ort_accel_name()
{
#ifdef _WIN32
	static std::string name;
	static std::once_flag once;
	std::call_once(once, [] {
		name = "DirectML";
		if (!g_dml_adapter_name.empty())
			name += " on " + g_dml_adapter_name;
	});
	return name.c_str();
#elif defined(__APPLE__)
	return "Core ML";
#else
	return "GPU";
#endif
}

} // namespace beauty
