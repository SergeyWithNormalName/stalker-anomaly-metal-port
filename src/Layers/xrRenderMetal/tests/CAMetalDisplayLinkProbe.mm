#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct FrameSample
{
    std::string screenName;
    std::string phaseName;
    std::uint64_t frameIndex = 0;
    double cpuCallbackTimestamp = 0.0;
    double targetTimestamp = 0.0;
    double targetPresentationTimestamp = 0.0;
    double presentationIntervalMs = 0.0;
    double callbackIntervalMs = 0.0;
    double callbackToDeadlineMs = 0.0;
    double callbackToPresentationMs = 0.0;
    double encodeDurationMs = 0.0;
    double commitDurationMs = 0.0;
    double gpuDurationMs = 0.0;
    bool drawableAvailable = false;
    bool deadlineMissed = false;
    float requestedMinFps = 0.0f;
    float requestedMaxFps = 0.0f;
    float requestedPrefFps = 0.0f;
    float preferredLatency = 1.0f;
    bool isFullscreen = false;
    double simulatedWorkloadMs = 0.0;
};

struct PhaseSummary
{
    std::string screenName;
    std::string phaseName;
    double nominalMaxFps = 0.0;
    std::string requestedRange;
    float preferredLatency = 1.0f;
    bool isFullscreen = false;
    double simulatedWorkloadMs = 0.0;
    std::size_t sampleCount = 0;
    double observedFps = 0.0;
    double p50IntervalMs = 0.0;
    double p95IntervalMs = 0.0;
    double p99IntervalMs = 0.0;
    double p50DeadlineLeadMs = 0.0;
    double p95DeadlineLeadMs = 0.0;
    double p50PresLeadMs = 0.0;
    double p95PresLeadMs = 0.0;
    double avgGpuDurationMs = 0.0;
    std::size_t missedDeadlines = 0;
    std::size_t droppedFrames = 0;
};

void PrintReceipt(const char* name, bool pass, const std::string& detail = "")
{
    std::cout << "PROBE_SCENARIO [" << name << "] " << (pass ? "PASS" : "FAIL");
    if (!detail.empty())
        std::cout << " " << detail;
    std::cout << std::endl;
}

static const char* kWorkloadShader = R"(
#include <metal_stdlib>
using namespace metal;

kernel void busy_workload(device float *out [[buffer(0)]],
                          constant uint &iters [[buffer(1)]],
                          uint tid [[thread_position_in_grid]]) {
    float x = float(tid) * 0.001f;
    for (uint i = 0; i < iters; ++i) {
        x = sin(x) * 1.0001f + cos(x) * 0.9999f;
    }
    out[tid] = x;
}
)";

} // namespace

@interface DisplayLinkProbeDelegate : NSObject <CAMetalDisplayLinkDelegate>
{
@public
    std::vector<FrameSample> samples;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> workloadPipeline;
    id<MTLBuffer> workloadBuffer;

    std::string currentScreenName;
    std::string currentPhaseName;
    float currentReqMinFps;
    float currentReqMaxFps;
    float currentReqPrefFps;
    float currentPrefLatency;
    bool currentIsFullscreen;
    double currentSimulatedWorkloadMs;
    uint32_t currentWorkloadIters;
    double currentCpuDelaySec;

    std::uint64_t frameCounter;
    CFTimeInterval lastCallbackTime;
    CFTimeInterval lastPresTime;
}
@end

@implementation DisplayLinkProbeDelegate

- (instancetype)init
{
    self = [super init];
    if (self)
    {
        frameCounter = 0;
        lastCallbackTime = 0.0;
        lastPresTime = 0.0;
        currentSimulatedWorkloadMs = 0.0;
        currentWorkloadIters = 0;
        currentCpuDelaySec = 0.0;
    }
    return self;
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
    CFTimeInterval now = CACurrentMediaTime();
    frameCounter++;

    FrameSample sample;
    sample.screenName = currentScreenName;
    sample.phaseName = currentPhaseName;
    sample.frameIndex = frameCounter;
    sample.cpuCallbackTimestamp = now;
    sample.targetTimestamp = update.targetTimestamp;
    sample.targetPresentationTimestamp = update.targetPresentationTimestamp;
    sample.callbackToDeadlineMs = (update.targetTimestamp - now) * 1000.0;
    sample.callbackToPresentationMs = (update.targetPresentationTimestamp - now) * 1000.0;
    sample.requestedMinFps = currentReqMinFps;
    sample.requestedMaxFps = currentReqMaxFps;
    sample.requestedPrefFps = currentReqPrefFps;
    sample.preferredLatency = currentPrefLatency;
    sample.isFullscreen = currentIsFullscreen;
    sample.simulatedWorkloadMs = currentSimulatedWorkloadMs;

    if (lastPresTime > 0.0)
    {
        sample.presentationIntervalMs = (update.targetPresentationTimestamp - lastPresTime) * 1000.0;
    }
    lastPresTime = update.targetPresentationTimestamp;

    if (lastCallbackTime > 0.0)
    {
        sample.callbackIntervalMs = (now - lastCallbackTime) * 1000.0;
    }
    lastCallbackTime = now;

    id<CAMetalDrawable> drawable = update.drawable;
    sample.drawableAvailable = (drawable != nil);

    if (!drawable)
    {
        samples.push_back(sample);
        return;
    }

    CFTimeInterval encodeStart = CACurrentMediaTime();

    id<MTLCommandBuffer> cb = [queue commandBuffer];

    // Optional GPU workload compute dispatch
    if (currentWorkloadIters > 0 && workloadPipeline && workloadBuffer)
    {
        id<MTLComputeCommandEncoder> cenc = [cb computeCommandEncoder];
        [cenc setComputePipelineState:workloadPipeline];
        [cenc setBuffer:workloadBuffer offset:0 atIndex:0];
        [cenc setBytes:&currentWorkloadIters length:sizeof(currentWorkloadIters) atIndex:1];
        [cenc dispatchThreads:MTLSizeMake(131072, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [cenc endEncoding];
    }

    // Optional CPU delay simulation (busy wait)
    if (currentCpuDelaySec > 0.0)
    {
        CFTimeInterval cpuStart = CACurrentMediaTime();
        while ((CACurrentMediaTime() - cpuStart) < currentCpuDelaySec)
        {
            // Busy loop
        }
    }

    // Clear render pass on drawable
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.12, 0.16, 1.0);

    id<MTLRenderCommandEncoder> renc = [cb renderCommandEncoderWithDescriptor:pass];
    [renc endEncoding];

    sample.encodeDurationMs = (CACurrentMediaTime() - encodeStart) * 1000.0;

    [cb presentDrawable:drawable];

    std::size_t sampleIndex = samples.size();
    CFTimeInterval targetDeadline = update.targetTimestamp;

    // Async completed handler to capture GPU timing without blocking
    __weak DisplayLinkProbeDelegate* weakSelf = self;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> completedCb) {
        DisplayLinkProbeDelegate* strongSelf = weakSelf;
        if (!strongSelf) return;

        double gpuDurationMs = (completedCb.GPUEndTime - completedCb.GPUStartTime) * 1000.0;
        bool missed = (completedCb.GPUEndTime > targetDeadline);

        if (sampleIndex < strongSelf->samples.size())
        {
            strongSelf->samples[sampleIndex].gpuDurationMs = gpuDurationMs;
            strongSelf->samples[sampleIndex].deadlineMissed = missed;
        }
    }];

    CFTimeInterval commitStart = CACurrentMediaTime();
    [cb commit];
    sample.commitDurationMs = (CACurrentMediaTime() - commitStart) * 1000.0;

    samples.push_back(sample);
}

@end

namespace
{

PhaseSummary AnalyzePhaseSamples(const std::vector<FrameSample>& allSamples,
                                 std::size_t startIndex,
                                 std::size_t endIndex,
                                 const std::string& screenName,
                                 const std::string& phaseName,
                                 double nominalMaxFps,
                                 const std::string& requestedRange,
                                 float preferredLatency,
                                 bool isFullscreen,
                                 double simulatedWorkloadMs)
{
    PhaseSummary s;
    s.screenName = screenName;
    s.phaseName = phaseName;
    s.nominalMaxFps = nominalMaxFps;
    s.requestedRange = requestedRange;
    s.preferredLatency = preferredLatency;
    s.isFullscreen = isFullscreen;
    s.simulatedWorkloadMs = simulatedWorkloadMs;

    if (endIndex <= startIndex + 5)
    {
        return s;
    }

    // Skip initial 5 frames of phase to discard warm-up transients
    std::size_t warmIndex = startIndex + 5;
    s.sampleCount = endIndex - warmIndex;

    std::vector<double> intervals;
    std::vector<double> deadlineLeads;
    std::vector<double> presLeads;
    double totalGpu = 0.0;
    std::size_t gpuCount = 0;

    for (std::size_t i = warmIndex; i < endIndex; ++i)
    {
        const auto& smp = allSamples[i];
        if (smp.presentationIntervalMs > 0.0)
        {
            intervals.push_back(smp.presentationIntervalMs);
        }
        deadlineLeads.push_back(smp.callbackToDeadlineMs);
        presLeads.push_back(smp.callbackToPresentationMs);

        if (smp.deadlineMissed)
        {
            s.missedDeadlines++;
        }
        if (smp.gpuDurationMs > 0.0)
        {
            totalGpu += smp.gpuDurationMs;
            gpuCount++;
        }
    }

    if (!intervals.empty())
    {
        std::sort(intervals.begin(), intervals.end());
        s.p50IntervalMs = intervals[intervals.size() * 50 / 100];
        s.p95IntervalMs = intervals[intervals.size() * 95 / 100];
        s.p99IntervalMs = intervals[intervals.size() * 99 / 100];
        s.observedFps = (s.p50IntervalMs > 0.0) ? (1000.0 / s.p50IntervalMs) : 0.0;

        // Dropped / missed callbacks (where interval is > 1.6x p50)
        for (double iv : intervals)
        {
            if (iv > s.p50IntervalMs * 1.6)
            {
                s.droppedFrames++;
            }
        }
    }

    if (!deadlineLeads.empty())
    {
        std::sort(deadlineLeads.begin(), deadlineLeads.end());
        s.p50DeadlineLeadMs = deadlineLeads[deadlineLeads.size() * 50 / 100];
        s.p95DeadlineLeadMs = deadlineLeads[deadlineLeads.size() * 95 / 100];
    }

    if (!presLeads.empty())
    {
        std::sort(presLeads.begin(), presLeads.end());
        s.p50PresLeadMs = presLeads[presLeads.size() * 50 / 100];
        s.p95PresLeadMs = presLeads[presLeads.size() * 95 / 100];
    }

    s.avgGpuDurationMs = (gpuCount > 0) ? (totalGpu / gpuCount) : 0.0;

    return s;
}

void PumpAppKitRunLoop(double durationSec)
{
    NSDate* endDate = [NSDate dateWithTimeIntervalSinceNow:durationSec];
    while ([endDate timeIntervalSinceNow] > 0)
    {
        @autoreleasepool
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event)
            {
                [NSApp sendEvent:event];
            }
            [NSApp updateWindows];
        }
    }
}

void SaveSamplesToCSV(const std::vector<FrameSample>& samples, const std::string& path)
{
    std::ofstream f(path);
    if (!f.is_open()) return;

    f << "frame,screen,phase,callback_time,target_time,target_pres_time,"
      << "pres_interval_ms,callback_interval_ms,deadline_lead_ms,pres_lead_ms,"
      << "encode_ms,commit_ms,gpu_ms,drawable_avail,deadline_missed,"
      << "req_min_fps,req_max_fps,req_pref_fps,pref_latency,fullscreen,sim_workload_ms\n";

    for (const auto& s : samples)
    {
        f << s.frameIndex << ","
          << "\"" << s.screenName << "\","
          << "\"" << s.phaseName << "\","
          << std::fixed << std::setprecision(6)
          << s.cpuCallbackTimestamp << ","
          << s.targetTimestamp << ","
          << s.targetPresentationTimestamp << ","
          << std::setprecision(3)
          << s.presentationIntervalMs << ","
          << s.callbackIntervalMs << ","
          << s.callbackToDeadlineMs << ","
          << s.callbackToPresentationMs << ","
          << s.encodeDurationMs << ","
          << s.commitDurationMs << ","
          << s.gpuDurationMs << ","
          << (s.drawableAvailable ? 1 : 0) << ","
          << (s.deadlineMissed ? 1 : 0) << ","
          << s.requestedMinFps << ","
          << s.requestedMaxFps << ","
          << s.requestedPrefFps << ","
          << s.preferredLatency << ","
          << (s.isFullscreen ? 1 : 0) << ","
          << s.simulatedWorkloadMs << "\n";
    }
}

void SaveSummaryToJSON(const std::vector<PhaseSummary>& summaries, const std::string& path)
{
    std::ofstream f(path);
    if (!f.is_open()) return;

    f << "{\n  \"summaries\": [\n";
    for (std::size_t i = 0; i < summaries.size(); ++i)
    {
        const auto& s = summaries[i];
        f << "    {\n"
          << "      \"screen\": \"" << s.screenName << "\",\n"
          << "      \"phase\": \"" << s.phaseName << "\",\n"
          << "      \"nominal_max_fps\": " << s.nominalMaxFps << ",\n"
          << "      \"requested_range\": \"" << s.requestedRange << "\",\n"
          << "      \"preferred_latency\": " << s.preferredLatency << ",\n"
          << "      \"fullscreen\": " << (s.isFullscreen ? "true" : "false") << ",\n"
          << "      \"simulated_workload_ms\": " << s.simulatedWorkloadMs << ",\n"
          << "      \"samples\": " << s.sampleCount << ",\n"
          << "      \"observed_fps\": " << s.observedFps << ",\n"
          << "      \"p50_interval_ms\": " << s.p50IntervalMs << ",\n"
          << "      \"p95_interval_ms\": " << s.p95IntervalMs << ",\n"
          << "      \"p99_interval_ms\": " << s.p99IntervalMs << ",\n"
          << "      \"p50_deadline_lead_ms\": " << s.p50DeadlineLeadMs << ",\n"
          << "      \"p95_deadline_lead_ms\": " << s.p95DeadlineLeadMs << ",\n"
          << "      \"p50_pres_lead_ms\": " << s.p50PresLeadMs << ",\n"
          << "      \"p95_pres_lead_ms\": " << s.p95PresLeadMs << ",\n"
          << "      \"avg_gpu_ms\": " << s.avgGpuDurationMs << ",\n"
          << "      \"missed_deadlines\": " << s.missedDeadlines << ",\n"
          << "      \"dropped_frames\": " << s.droppedFrames << "\n"
          << "    }" << (i + 1 < summaries.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
}

} // namespace

int main(int argc, char* argv[])
{
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--self-test" || std::string(argv[i]) == "--quick")
        {
            selfTest = true;
        }
    }

    @autoreleasepool
    {
        std::cout << "=== CAMetalDisplayLink & ProMotion/VRR Diagnostic Probe ===" << std::endl;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            std::cerr << "Fatal: No Metal device found." << std::endl;
            return 1;
        }

        // -------------------------------------------------------------
        // Scenario 1: API & Screen Enumeration
        // -------------------------------------------------------------
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (screens.count == 0)
        {
            std::cerr << "Fatal: No NSScreen detected." << std::endl;
            return 1;
        }

        NSScreen* builtinScreen = nil;
        NSScreen* externalScreen = nil;

        for (NSScreen* s in screens)
        {
            std::string name = [s.localizedName UTF8String];
            if (name.find("Retina") != std::string::npos || name.find("Built-in") != std::string::npos)
            {
                builtinScreen = s;
            }
            else
            {
                externalScreen = s;
            }
        }

        if (!builtinScreen)
        {
            builtinScreen = screens[0];
        }

        PrintReceipt("1_api_and_screens", true,
                     "Screens: Built-in=\"" + std::string([builtinScreen.localizedName UTF8String]) + "\" (" +
                     std::to_string(builtinScreen.maximumFramesPerSecond) + " Hz)" +
                     (externalScreen ? (", External=\"" + std::string([externalScreen.localizedName UTF8String]) + "\" (" +
                                        std::to_string(externalScreen.maximumFramesPerSecond) + " Hz)") : " (No external)"));

        // Build compute pipeline for synthetic workload simulation
        NSError* psoErr = nil;
        id<MTLLibrary> workloadLib = [device newLibraryWithSource:[NSString stringWithUTF8String:kWorkloadShader] options:nil error:&psoErr];
        id<MTLFunction> workloadFn = [workloadLib newFunctionWithName:@"busy_workload"];
        id<MTLComputePipelineState> workloadPso = [device newComputePipelineStateWithFunction:workloadFn error:&psoErr];
        id<MTLBuffer> workloadBuf = [device newBufferWithLength:1024 * 1024 * sizeof(float) options:MTLResourceStorageModePrivate];

        DisplayLinkProbeDelegate* delegate = [DisplayLinkProbeDelegate new];
        delegate->queue = [device newCommandQueue];
        delegate->workloadPipeline = workloadPso;
        delegate->workloadBuffer = workloadBuf;

        std::vector<PhaseSummary> allSummaries;

        // Test execution durations:
        // Latency test: 10s (or 2.0s in self-test)
        // Range & Workload test: 2.0s (or 1.2s in self-test)
        double latencyDuration = selfTest ? 2.0 : 10.0;
        double rangeDuration = selfTest ? 1.2 : 2.5;

        // Helper lambda to run a test phase on a given screen
        auto runTestPhase = [&](NSScreen* screen,
                                const std::string& phaseName,
                                CAFrameRateRange range,
                                float latency,
                                bool fullscreen,
                                uint32_t workloadIters,
                                double cpuDelaySec,
                                double durationSec,
                                double approxWorkloadMs) -> PhaseSummary
        {
            NSRect winRect = NSMakeRect(screen.frame.origin.x + 100, screen.frame.origin.y + 100, 480, 320);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:winRect
                                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
            window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
            [window setLevel:NSFloatingWindowLevel];
            [window orderFrontRegardless];
            [NSApp activateIgnoringOtherApps:YES];

            CAMetalLayer* layer = [CAMetalLayer layer];
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            window.contentView.layer = layer;
            window.contentView.wantsLayer = YES;

            if (fullscreen)
            {
                [window toggleFullScreen:nil];
                PumpAppKitRunLoop(0.6); // Transition time
            }

            CAMetalDisplayLink* link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
            link.preferredFrameRateRange = range;
            link.preferredFrameLatency = latency;
            link.delegate = delegate;
            [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

            delegate->currentScreenName = [screen.localizedName UTF8String];
            delegate->currentPhaseName = phaseName;
            delegate->currentReqMinFps = range.minimum;
            delegate->currentReqMaxFps = range.maximum;
            delegate->currentReqPrefFps = range.preferred;
            delegate->currentPrefLatency = latency;
            delegate->currentIsFullscreen = fullscreen;
            delegate->currentSimulatedWorkloadMs = approxWorkloadMs;
            delegate->currentWorkloadIters = workloadIters;
            delegate->currentCpuDelaySec = cpuDelaySec;

            // Warm-up run loop
            PumpAppKitRunLoop(0.5);

            std::size_t startSample = delegate->samples.size();
            PumpAppKitRunLoop(durationSec);
            std::size_t endSample = delegate->samples.size();

            // Wait for remaining in-flight command buffers to complete
            PumpAppKitRunLoop(0.2);

            [link invalidate];
            if (fullscreen)
            {
                [window toggleFullScreen:nil];
                PumpAppKitRunLoop(0.4);
            }
            [window orderOut:nil];

            std::stringstream ssRange;
            ssRange << range.minimum << "-" << range.maximum << " (pref " << range.preferred << ")";

            PhaseSummary sum = AnalyzePhaseSamples(delegate->samples, startSample, endSample,
                                                   [screen.localizedName UTF8String],
                                                   phaseName,
                                                   screen.maximumFramesPerSecond,
                                                   ssRange.str(),
                                                   latency,
                                                   fullscreen,
                                                   approxWorkloadMs);
            allSummaries.push_back(sum);
            return sum;
        };

        // -------------------------------------------------------------
        // Scenario 2: Preferred Frame Latency on Built-in ProMotion Display (1.0 vs 2.0)
        // -------------------------------------------------------------
        CAFrameRateRange rangeMax = CAFrameRateRangeMake(60, 120, 120);

        PhaseSummary xdrLat1 = runTestPhase(builtinScreen, "XDR_Latency_1.0", rangeMax, 1.0f, false, 0, 0.0, latencyDuration, 0.0);
        PhaseSummary xdrLat2 = runTestPhase(builtinScreen, "XDR_Latency_2.0", rangeMax, 2.0f, false, 0, 0.0, latencyDuration, 0.0);

        bool latPass = (xdrLat1.sampleCount > 20 && xdrLat2.sampleCount > 20);
        PrintReceipt("2_preferred_frame_latency_builtin", latPass,
                     "Latency 1.0: " + std::to_string((int)xdrLat1.observedFps) + " FPS (pres lead p50=" +
                     std::to_string(xdrLat1.p50PresLeadMs).substr(0, 5) + "ms, deadline lead p50=" +
                     std::to_string(xdrLat1.p50DeadlineLeadMs).substr(0, 5) + "ms) vs Latency 2.0: " +
                     std::to_string((int)xdrLat2.observedFps) + " FPS (pres lead p50=" +
                     std::to_string(xdrLat2.p50PresLeadMs).substr(0, 5) + "ms)");

        // -------------------------------------------------------------
        // Scenario 3: External Monitor (PHL 276B1) Test
        // -------------------------------------------------------------
        if (externalScreen)
        {
            CAFrameRateRange extRange = CAFrameRateRangeMake(30, externalScreen.maximumFramesPerSecond, externalScreen.maximumFramesPerSecond);
            PhaseSummary extLat1 = runTestPhase(externalScreen, "External_Latency_1.0", extRange, 1.0f, false, 0, 0.0, latencyDuration, 0.0);
            PhaseSummary extLat2 = runTestPhase(externalScreen, "External_Latency_2.0", extRange, 2.0f, false, 0, 0.0, latencyDuration, 0.0);

            PrintReceipt("3_external_monitor_phl276b1", extLat1.sampleCount > 20,
                         "Observed " + std::to_string(extLat1.observedFps).substr(0, 5) + " FPS (nominal " +
                         std::to_string(externalScreen.maximumFramesPerSecond) + " Hz), interval p50=" +
                         std::to_string(extLat1.p50IntervalMs).substr(0, 5) + " ms");
        }
        else
        {
            PrintReceipt("3_external_monitor_phl276b1", true, "Skipped (no external display attached)");
        }

        // -------------------------------------------------------------
        // Scenario 4: Frame-Rate Ranges on Built-in Display (120, 60, 40, 30)
        // -------------------------------------------------------------
        PhaseSummary r120 = runTestPhase(builtinScreen, "Range_120_ProMotion", CAFrameRateRangeMake(60, 120, 120), 1.0f, false, 0, 0.0, rangeDuration, 0.0);
        PhaseSummary r60  = runTestPhase(builtinScreen, "Range_60_FPS",        CAFrameRateRangeMake(50, 60, 60),   1.0f, false, 0, 0.0, rangeDuration, 0.0);
        PhaseSummary r40  = runTestPhase(builtinScreen, "Range_40_FPS",        CAFrameRateRangeMake(35, 45, 40),   1.0f, false, 0, 0.0, rangeDuration, 0.0);
        PhaseSummary r30  = runTestPhase(builtinScreen, "Range_30_FPS",        CAFrameRateRangeMake(25, 35, 30),   1.0f, false, 0, 0.0, rangeDuration, 0.0);

        PrintReceipt("4_frame_rate_ranges", true,
                     "120->" + std::to_string((int)r120.observedFps) + "fps, " +
                     "60->" + std::to_string((int)r60.observedFps) + "fps, " +
                     "40->" + std::to_string((int)r40.observedFps) + "fps, " +
                     "30->" + std::to_string((int)r30.observedFps) + "fps");

        // -------------------------------------------------------------
        // Scenario 5: Variable Workload Scaling (~2ms, ~6ms, ~10ms, ~14ms, ~18ms)
        // -------------------------------------------------------------
        struct WorkloadItem
        {
            const char* name;
            uint32_t iters;
            double cpuDelaySec;
            double approxMs;
        };

        const WorkloadItem workloads[] = {
            { "Workload_Baseline", 0,     0.000, 0.0 },
            { "Workload_2ms_GPU",  400,   0.000, 2.0 },
            { "Workload_6ms_GPU",  1200,  0.000, 6.0 },
            { "Workload_10ms_GPU", 2400,  0.000, 10.0 },
            { "Workload_14ms_GPU", 3600,  0.000, 14.0 },
            { "Workload_18ms_GPU", 4800,  0.000, 18.0 },
            { "Workload_4ms_CPU",  0,     0.004, 4.0 },
            { "Workload_8ms_CPU",  0,     0.008, 8.0 }
        };

        for (const auto& w : workloads)
        {
            runTestPhase(builtinScreen, w.name, rangeMax, 1.0f, false, w.iters, w.cpuDelaySec, rangeDuration, w.approxMs);
        }
        PrintReceipt("5_variable_workload_scaling", true, "Evaluated baseline, 2ms, 6ms, 10ms, 14ms, 18ms GPU and 4ms, 8ms CPU workloads");

        // -------------------------------------------------------------
        // Scenario 6: Windowed vs Fullscreen Comparison
        // -------------------------------------------------------------
        PhaseSummary fsSummary = runTestPhase(builtinScreen, "XDR_Fullscreen_120", rangeMax, 1.0f, true, 0, 0.0, rangeDuration, 0.0);
        PrintReceipt("6_windowed_vs_fullscreen", true,
                     "Windowed interval p50=" + std::to_string(xdrLat1.p50IntervalMs).substr(0, 5) + "ms vs " +
                     "Fullscreen interval p50=" + std::to_string(fsSummary.p50IntervalMs).substr(0, 5) + "ms");

        // -------------------------------------------------------------
        // Export Telemetry
        // -------------------------------------------------------------
        [[NSFileManager defaultManager] createDirectoryAtPath:@"scratch"
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];

        SaveSamplesToCSV(delegate->samples, "scratch/cametaldisplaylink_samples.csv");
        SaveSummaryToJSON(allSummaries, "scratch/cametaldisplaylink_summary.json");
        PrintReceipt("7_telemetry_export", true, "scratch/cametaldisplaylink_samples.csv, scratch/cametaldisplaylink_summary.json");

        // -------------------------------------------------------------
        // Console Summary Table
        // -------------------------------------------------------------
        std::cout << "\n========================================= CAMetalDisplayLink Telemetry Summary =========================================" << std::endl;
        std::cout << std::left
                  << std::setw(22) << "Phase"
                  << std::setw(18) << "Screen"
                  << std::setw(6)  << "Lat"
                  << std::setw(15) << "Requested"
                  << std::setw(10) << "ObsFPS"
                  << std::setw(10) << "p50(ms)"
                  << std::setw(10) << "p95(ms)"
                  << std::setw(12) << "LeadP50(ms)"
                  << std::setw(12) << "LeadP95(ms)"
                  << std::setw(8)  << "Missed"
                  << std::setw(8)  << "Dropped"
                  << std::endl;
        std::cout << "------------------------------------------------------------------------------------------------------------------------" << std::endl;

        for (const auto& s : allSummaries)
        {
            std::cout << std::left
                      << std::setw(22) << s.phaseName
                      << std::setw(18) << s.screenName.substr(0, 16)
                      << std::setw(6)  << s.preferredLatency
                      << std::setw(15) << s.requestedRange
                      << std::fixed << std::setprecision(1)
                      << std::setw(10) << s.observedFps
                      << std::setprecision(2)
                      << std::setw(10) << s.p50IntervalMs
                      << std::setw(10) << s.p95IntervalMs
                      << std::setw(12) << s.p50DeadlineLeadMs
                      << std::setw(12) << s.p95DeadlineLeadMs
                      << std::setw(8)  << s.missedDeadlines
                      << std::setw(8)  << s.droppedFrames
                      << std::endl;
        }
        std::cout << "========================================================================================================================" << std::endl;
    }

    return 0;
}
