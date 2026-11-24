// ortbench <model.onnx> <config>  -- times one model under one execution-provider configuration
#include <onnxruntime_cxx_api.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
int main(int argc, char **argv)
{
	std::string path = argv[1], cfg = argv[2];
	Ort::Env env(cfg.find("verbose") != std::string::npos ? ORT_LOGGING_LEVEL_VERBOSE : ORT_LOGGING_LEVEL_WARNING, "b");
	Ort::SessionOptions so;
	so.SetIntraOpNumThreads(2);
	so.AddConfigEntry("session.intra_op.allow_spinning", "0");
	so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
	if (cfg.rfind("coreml", 0) == 0) {
		std::unordered_map<std::string, std::string> o;
		o["ModelFormat"] = cfg.find("mlprogram") != std::string::npos ? "MLProgram" : "NeuralNetwork";
		if (cfg.find("ane") != std::string::npos) o["MLComputeUnits"] = "CPUAndNeuralEngine";
		if (cfg.find("gpu") != std::string::npos) o["MLComputeUnits"] = "CPUAndGPU";
		if (cfg.find("all") != std::string::npos) o["MLComputeUnits"] = "ALL";
		if (cfg.find("static") != std::string::npos) o["RequireStaticInputShapes"] = "1";
		so.AppendExecutionProvider("CoreML", o);
	}
	auto t0 = std::chrono::steady_clock::now();
	Ort::Session s(env, path.c_str(), so);
	double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	Ort::AllocatorWithDefaultOptions al;
	auto in_name = s.GetInputNameAllocated(0, al);
	auto shape = s.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
	for (auto &d : shape) if (d < 0) d = 1;
	size_t n = 1; for (auto d : shape) n *= d;
	std::vector<float> in(n, 0.5f);
	std::vector<std::string> outs; std::vector<const char *> outp;
	for (size_t i = 0; i < s.GetOutputCount(); i++) outs.push_back(s.GetOutputNameAllocated(i, al).get());
	for (auto &o : outs) outp.push_back(o.c_str());
	const char *inp[] = {in_name.get()};
	auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	Ort::Value t = Ort::Value::CreateTensor<float>(mem, in.data(), n, shape.data(), shape.size());
	for (int i = 0; i < 10; i++) s.Run(Ort::RunOptions{nullptr}, inp, &t, 1, outp.data(), outp.size());
	std::vector<double> ts;
	for (int i = 0; i < 100; i++) {
		auto a = std::chrono::steady_clock::now();
		s.Run(Ort::RunOptions{nullptr}, inp, &t, 1, outp.data(), outp.size());
		ts.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
	}
	std::sort(ts.begin(), ts.end());
	double sum = 0; for (double x : ts) sum += x;
	printf("%-28s load %6.0f ms   run mean %6.2f  median %6.2f  p95 %6.2f ms\n", cfg.c_str(), load_ms, sum / ts.size(), ts[50], ts[95]);
}
