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
#ifdef BEAUTY_HAVE_LIBOBS
	blog(LOG_INFO, "[dewy] %s", msg);
#else
	fprintf(stderr, "[dewy] %s\n", msg);
#endif
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

static OrtModelPtr load_model(const std::string &path, bool prefer_accel)
{
	std::vector<char> bytes = read_file(path);
	if (bytes.empty()) {
		ort_log("missing model %s", path);
		return nullptr;
	}
	auto m = std::make_shared<OrtModel>();
	auto build = [&](bool accel) {
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
				/* NeuralNetwork format: MLProgram rejects some MediaPipe depthwise convs */
				so.AppendExecutionProvider("CoreML", {{"ModelFormat", "NeuralNetwork"}});
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
		build(prefer_accel);
	} catch (const Ort::Exception &e) {
		if (!prefer_accel)
			throw;
		ort_log("accelerated provider failed (%s), using CPU", e.what());
		build(false);
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
