// cmbench <model.mlmodelc> <input size> <all|gpu|ane|cpu> [runs]: Core ML speed, CPU per run and where
// each op runs (MLComputePlan). Compile a model first: xcrun coremlcompiler compile x.mlpackage .
//   clang++ -std=c++17 -O2 -fobjc-arc cmbench.mm -framework CoreML -framework CoreVideo -framework Foundation -o cmbench
#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#include <sys/resource.h>
#include <mach/mach_time.h>
#include <vector>
#include <algorithm>
#include <cstdio>

static double now_ms() { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6; }
static double cpu_ms() { struct rusage r; getrusage(RUSAGE_SELF, &r);
	return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3; }

int main(int argc, char **argv) {
	@autoreleasepool {
		NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
		int S = atoi(argv[2]);
		NSString *u = @(argv[3]);
		int runs = argc > 4 ? atoi(argv[4]) : 200;
		MLModelConfiguration *cfg = [MLModelConfiguration new];
		cfg.computeUnits = [u isEqual:@"gpu"] ? MLComputeUnitsCPUAndGPU : [u isEqual:@"ane"] ? MLComputeUnitsCPUAndNeuralEngine
				 : [u isEqual:@"cpu"] ? MLComputeUnitsCPUOnly : MLComputeUnitsAll;
		NSError *err = nil;
		double t0 = now_ms();
		MLModel *model = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
		if (!model) { printf("load failed: %s\n", err.description.UTF8String); return 1; }
		double tload = now_ms() - t0;

		// op placement
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);
		__block NSMutableDictionary<NSString *, NSNumber *> *count = [NSMutableDictionary new], *cost = [NSMutableDictionary new];
		[MLComputePlan loadContentsOfURL:url configuration:cfg completionHandler:^(MLComputePlan *plan, NSError *e) {
			if (!plan) { printf("plan failed: %s\n", e.description.UTF8String); dispatch_semaphore_signal(sem); return; }
			MLModelStructureProgramFunction *fn = plan.modelStructure.program.functions[@"main"];
			for (MLModelStructureProgramOperation *op in fn.block.operations) {
				MLComputePlanDeviceUsage *du = [plan computeDeviceUsageForMLProgramOperation:op];
				if (!du) continue; // consts etc.
				NSString *d = [du.preferredComputeDevice isKindOfClass:MLNeuralEngineComputeDevice.class] ? @"ANE"
					: [du.preferredComputeDevice isKindOfClass:MLGPUComputeDevice.class] ? @"GPU" : @"CPU";
				MLComputePlanCost *c = [plan estimatedCostOfMLProgramOperation:op];
				count[d] = @(count[d].intValue + 1);
				cost[d] = @(cost[d].doubleValue + (c ? c.weight : 0));
			}
			dispatch_semaphore_signal(sem);
		}];
		dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
		NSMutableString *pl = [NSMutableString new];
		for (NSString *d in @[@"ANE", @"GPU", @"CPU"]) if (count[d]) [pl appendFormat:@"%@ %d ops (%.0f%% cost)  ", d, count[d].intValue, 100 * cost[d].doubleValue];

		// input: BGRA pixel buffer with noise-ish content
		CVPixelBufferRef pb = nullptr;
		NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
		CVPixelBufferCreate(nullptr, S, S, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &pb);
		CVPixelBufferLockBaseAddress(pb, 0);
		uint8_t *p = (uint8_t *)CVPixelBufferGetBaseAddress(pb); size_t st = CVPixelBufferGetBytesPerRow(pb);
		for (int y = 0; y < S; y++) for (int x = 0; x < S * 4; x++) p[y * st + x] = (uint8_t)((x * 7 + y * 13) ^ (x * y));
		CVPixelBufferUnlockBaseAddress(pb, 0);
		MLDictionaryFeatureProvider *in = [[MLDictionaryFeatureProvider alloc]
			initWithDictionary:@{@"image": [MLFeatureValue featureValueWithPixelBuffer:pb]} error:&err];

		t0 = now_ms();
		[model predictionFromFeatures:in error:&err];
		double tfirst = now_ms() - t0;
		for (int i = 0; i < 20; i++) @autoreleasepool { [model predictionFromFeatures:in error:&err]; }
		std::vector<double> t;
		double c0 = cpu_ms(), w0 = now_ms();
		for (int i = 0; i < runs; i++) @autoreleasepool {
			double a = now_ms();
			id<MLFeatureProvider> o = [model predictionFromFeatures:in error:&err];
			if (!o) { printf("predict failed: %s\n", err.description.UTF8String); return 1; }
			t.push_back(now_ms() - a);
		}
		double cpu = (cpu_ms() - c0) / runs, wall = (now_ms() - w0) / runs;
		std::sort(t.begin(), t.end());
		printf("%-4s mean %6.2f  med %6.2f  p95 %6.2f ms | CPU %5.2f ms/run | load %.0f ms, 1st %.0f ms | %s\n", u.UTF8String,
		       wall, t[t.size() / 2], t[t.size() * 95 / 100], cpu, tload, tfirst, pl.UTF8String);
		CVPixelBufferRelease(pb);
	}
	return 0;
}
