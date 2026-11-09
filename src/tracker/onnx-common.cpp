#include "onnx-common.hpp"

#ifdef _WIN32
#include <dml_provider_factory.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

#ifdef BEAUTY_HAVE_LIBOBS
#include <util/base.h>
#endif

static std::string g_ort_library;
extern "C" void face_tracker_set_runtime_library(const char *path)
{
	g_ort_library = path ? path : "";
}

namespace beauty {

void ort_log(const char *fmt, const std::string &arg)
{
	char msg[512];
	snprintf(msg, sizeof(msg), fmt, arg.c_str());
#ifdef BEAUTY_HAVE_LIBOBS
	blog(LOG_INFO, "[obs-beauty] %s", msg);
#else
	fprintf(stderr, "[obs-beauty] %s\n", msg);
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
	static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "obs-beauty");
	return env;
}

static std::vector<char> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

OrtModel ort_model(const std::string &path, bool prefer_accel)
{
	OrtModel m;
	if (!ort_load())
		return m;
	std::vector<char> bytes = read_file(path);
	if (bytes.empty()) {
		ort_log("missing model %s", path);
		return m;
	}
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
				on = dml->SessionOptionsAppendExecutionProvider_DML(so, 0) == nullptr;
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
		m.session = Ort::Session(ort_env(), bytes.data(), bytes.size(), so);
		m.accelerated = on;
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
	for (size_t i = 0; i < m.session.GetInputCount(); i++)
		m.in_names.push_back(m.session.GetInputNameAllocated(i, alloc).get());
	for (size_t i = 0; i < m.session.GetOutputCount(); i++)
		m.out_names.push_back(m.session.GetOutputNameAllocated(i, alloc).get());
	for (auto &s : m.in_names)
		m.in.push_back(s.c_str());
	for (auto &s : m.out_names)
		m.out.push_back(s.c_str());
	return m;
}

} // namespace beauty
