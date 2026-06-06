#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/capture_session.h"

#include "../include/fern/buffer.h"
#include "../include/fern/capture.h"
#include "../include/fern/capture_feedback.h"
#include "../include/fern/clock.h"
#include "../include/fern/encoder.h"
#include "../include/fern/ipc_server.h"
#include "../include/fern/multi_app_audio_capture.h"
#include "../include/fern/replay_export.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mferror.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fern {
namespace {

constexpr int kMaxCatchUpFramesPerLoop = 4;
constexpr LONGLONG kSaveAudioSettleHns = 2500000LL;
constexpr LONGLONG kDuplicationReconnectIntervalHns = HnsPerSecond / 2;

struct DesktopCaptureTarget {
    D3D11Context d3d;
    ComPtr<IDXGIOutputDuplication> duplication;
    DXGI_OUTDUPL_DESC duplicationDesc{};
    std::wstring outputName;
};

enum class DesktopFrameResult {
    Updated,
    Unchanged,
    AccessLost,
    Failed
};

void EnableD3D11MultithreadProtection(ID3D11Device* device) {
    if (!device) return;

    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }
}

HRESULT CreateD3D11DeviceForAdapter(IDXGIAdapter1* adapter, D3D11Context& d3d) {
    d3d = {};
    if (!adapter) return E_POINTER;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        creationFlags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3d.device,
        &featureLevel,
        &d3d.deviceContext);

    if (SUCCEEDED(hr)) {
        EnableD3D11MultithreadProtection(d3d.device.Get());
    }

    return hr;
}

bool TryCreateCopyTexture(ID3D11Device* device, UINT width, UINT height, ComPtr<ID3D11Texture2D>& texture) {
    texture.Reset();
    if (!device || width == 0 || height == 0) return false;

    D3D11_TEXTURE2D_DESC copyDesc = {};
    copyDesc.Width = width;
    copyDesc.Height = height;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    const HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, &texture);
    if (FAILED(hr) || !texture) {
        std::cerr << "VIDEO: copy texture creation failed 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    return true;
}

bool TryCreateDesktopCaptureTarget(DesktopCaptureTarget& target) {
    target = {};

    auto factory = getFactory1();
    auto adapters = getAdapters1(factory.Get());
    if (adapters.empty()) {
        std::cerr << "DXGI: no adapter found." << std::endl;
        return false;
    }

    for (const auto& adapter : adapters) {
        if (!adapter) continue;

        auto outputs = getOutputs1(adapter.Get());
        if (outputs.empty()) continue;

        D3D11Context d3d;
        HRESULT hr = CreateD3D11DeviceForAdapter(adapter.Get(), d3d);
        if (FAILED(hr) || !d3d.device || !d3d.deviceContext) {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);
            std::wcerr << L"D3D11: device creation failed for " << adapterDesc.Description
                       << L" 0x" << std::hex << hr << std::dec << std::endl;
            continue;
        }

        for (const auto& output : outputs) {
            if (!output) continue;

            ComPtr<IDXGIOutputDuplication> duplication;
            hr = output->DuplicateOutput(d3d.device.Get(), &duplication);
            if (SUCCEEDED(hr) && duplication) {
                DXGI_OUTPUT_DESC outputDesc{};
                output->GetDesc(&outputDesc);
                duplication->GetDesc(&target.duplicationDesc);
                target.d3d = d3d;
                target.duplication = duplication;
                target.outputName = outputDesc.DeviceName;
                return true;
            }

            DXGI_OUTPUT_DESC outputDesc{};
            output->GetDesc(&outputDesc);
            std::wcerr << L"DXGI: DuplicateOutput failed for " << outputDesc.DeviceName
                       << L" 0x" << std::hex << hr << std::dec << std::endl;
        }
    }

    return false;
}

bool TryCreateDuplicationOnCurrentDevice(
    ID3D11Device* device,
    const std::wstring& preferredOutputName,
    ComPtr<IDXGIOutputDuplication>& duplication,
    DXGI_OUTDUPL_DESC& duplicationDesc,
    std::wstring& outputName) {
    duplication.Reset();
    duplicationDesc = {};
    outputName.clear();
    if (!device) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) return false;

    struct CandidateOutput {
        ComPtr<IDXGIOutput1> output;
        std::wstring name;
    };

    std::vector<CandidateOutput> candidates;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(index, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !output) continue;

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1)) || !output1) continue;

        DXGI_OUTPUT_DESC outputDesc{};
        output->GetDesc(&outputDesc);
        candidates.push_back({ output1, outputDesc.DeviceName });
    }

    auto tryCandidate = [&](const CandidateOutput& candidate) {
        if (!candidate.output) return false;

        ComPtr<IDXGIOutputDuplication> candidateDuplication;
        HRESULT duplicateHr = candidate.output->DuplicateOutput(device, &candidateDuplication);
        if (FAILED(duplicateHr) || !candidateDuplication) return false;

        candidateDuplication->GetDesc(&duplicationDesc);
        duplication = candidateDuplication;
        outputName = candidate.name;
        return true;
    };

    if (!preferredOutputName.empty()) {
        for (const auto& candidate : candidates) {
            if (candidate.name == preferredOutputName && tryCandidate(candidate)) return true;
        }
    }

    for (const auto& candidate : candidates) {
        if (tryCandidate(candidate)) return true;
    }

    return false;
}

DesktopFrameResult TryUpdateDesktopFrame(IDXGIOutputDuplication* duplication, ID3D11DeviceContext* context, ID3D11Texture2D* target) {
    if (!duplication || !context || !target) return DesktopFrameResult::Failed;

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return DesktopFrameResult::Unchanged;
    if (hr == DXGI_ERROR_ACCESS_LOST) return DesktopFrameResult::AccessLost;
    if (FAILED(hr)) {
        std::cerr << "VIDEO: AcquireNextFrame failed 0x" << std::hex << hr << std::dec << std::endl;
        return DesktopFrameResult::Failed;
    }

    bool copied = false;
    if (resource) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture)) && texture) {
            context->CopyResource(target, texture.Get());
            copied = true;
        }
    }

    duplication->ReleaseFrame();
    return copied ? DesktopFrameResult::Updated : DesktopFrameResult::Unchanged;
}

void ClearTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    if (!device || !context || !texture) return;

    ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &rtv)) && rtv) {
        const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearRenderTargetView(rtv.Get(), black);
    }
}

bool SameDesktopDimensions(const DXGI_OUTDUPL_DESC& a, const DXGI_OUTDUPL_DESC& b) {
    return a.ModeDesc.Width == b.ModeDesc.Width && a.ModeDesc.Height == b.ModeDesc.Height;
}

bool BeginVideoEncoderStreaming(IMFTransform* videoEncoder) {
    if (!videoEncoder) return false;

    HRESULT hr = videoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;

    hr = videoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return SUCCEEDED(hr);
}

VideoEncoderSettings BuildVideoEncoderSettings(const Settings& settings) {
    VideoEncoderSettings encoderSettings;
    encoderSettings.fps = settings.fps;
    encoderSettings.bitrateMbps = settings.bitrate;
    encoderSettings.codec = settings.videoCodec;
    encoderSettings.profile = settings.encoderProfile;
    encoderSettings.rateControl = settings.rateControl;
    encoderSettings.maxBitrateMultiplier = settings.maxBitrateMultiplier;
    encoderSettings.gopSeconds = settings.gopSeconds;
    encoderSettings.bFrames = settings.bFrames;
    encoderSettings.lowLatency = settings.lowLatency;
    encoderSettings.qualityVsSpeed = settings.qualityVsSpeed;
    encoderSettings.encoderIndex = settings.encoderIndex;
    return encoderSettings;
}

bool RebuildVideoPipeline(
    const Settings& settings,
    DesktopCaptureTarget& captureTarget,
    DXGIDeviceManagerAndUInt& dxgiDeviceManager,
    ComPtr<IMFTransform>& videoEncoder,
    ComPtr<IMFMediaEventGenerator>& videoEventGen,
    ComPtr<ID3D11Texture2D>& copyTexture) {
    DesktopCaptureTarget rebuiltTarget;
    if (!TryCreateDesktopCaptureTarget(rebuiltTarget)) {
        std::cerr << "VIDEO: desktop duplication rebuild failed." << std::endl;
        return false;
    }

    auto rebuiltDeviceManager = CreateDXGIDeviceManager(rebuiltTarget.d3d.device.Get());
    if (!rebuiltDeviceManager.deviceManager) {
        std::cerr << "DXGI: device manager rebuild failed." << std::endl;
        return false;
    }

    ComPtr<IMFTransform> rebuiltEncoder;
    HRESULT hr = InitializeHardwareEncoder(
        rebuiltDeviceManager.deviceManager.Get(),
        rebuiltEncoder,
        rebuiltTarget.duplicationDesc.ModeDesc.Width,
        rebuiltTarget.duplicationDesc.ModeDesc.Height,
        BuildVideoEncoderSettings(settings));
    if (FAILED(hr) || !rebuiltEncoder) {
        std::cerr << "VIDEO: encoder rebuild failed 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    ComPtr<ID3D11Texture2D> rebuiltCopyTexture;
    if (!TryCreateCopyTexture(
            rebuiltTarget.d3d.device.Get(),
            rebuiltTarget.duplicationDesc.ModeDesc.Width,
            rebuiltTarget.duplicationDesc.ModeDesc.Height,
            rebuiltCopyTexture)) {
        return false;
    }
    ClearTexture(rebuiltTarget.d3d.device.Get(), rebuiltTarget.d3d.deviceContext.Get(), rebuiltCopyTexture.Get());

    if (!BeginVideoEncoderStreaming(rebuiltEncoder.Get())) {
        std::cerr << "VIDEO: encoder streaming restart failed." << std::endl;
        return false;
    }

    ComPtr<IMFMediaEventGenerator> rebuiltEventGen;
    rebuiltEncoder.As(&rebuiltEventGen);

    captureTarget = rebuiltTarget;
    dxgiDeviceManager = rebuiltDeviceManager;
    videoEncoder = rebuiltEncoder;
    videoEventGen = rebuiltEventGen;
    copyTexture = rebuiltCopyTexture;
    return true;
}

void DrainVideoEncoder(IMFTransform* encoder, IMFMediaEventGenerator* eventGen, RingBuffer& ringBuffer, int& framesProduced, int targetFps) {
    if (!encoder) return;

    auto storeOutput = [&](IMFSample* output) {
        ++framesProduced;
        ringBuffer.AddSample(output, 0);
        if (targetFps > 0 && framesProduced % targetFps == 0) {
            std::cout << ">>> Status | Buffer: " << ringBuffer.GetSampleCount() << " samples" << std::endl;
        }
    };

    if (!eventGen) {
        for (;;) {
            ComPtr<IMFSample> output;
            HRESULT hr = PullSampleFromEncoder(encoder, output);
            if (hr != S_OK || !output) break;
            storeOutput(output.Get());
        }
        return;
    }

    for (;;) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (FAILED(hr) || !event) break;

        MediaEventType type = MEUnknown;
        if (SUCCEEDED(event->GetType(&type)) && type == METransformHaveOutput) {
            ComPtr<IMFSample> output;
            hr = PullSampleFromEncoder(encoder, output);
            if (hr == S_OK && output) storeOutput(output.Get());
        }
    }
}

void WaitBriefly(HANDLE timer, LONGLONG nowHns, LONGLONG nextFrameDueHns) {
    LONGLONG waitHns = nextFrameDueHns - nowHns;
    if (waitHns <= 0) return;

    waitHns = std::min<LONGLONG>(waitHns, 10000LL);
    if (timer) {
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -waitHns;
        if (SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }

    std::this_thread::sleep_for(std::chrono::microseconds(250));
}

void StartAsyncSave(
    RingBuffer& ringBuffer,
    IMFTransform* videoEncoder,
    const MultiAppAudioCapture& audioCapture,
    const std::wstring& storagePath,
    LONGLONG exportEndHns) {
    ComPtr<IMFMediaType> videoType;
    videoEncoder->GetOutputCurrentType(0, &videoType);

    std::vector<ComPtr<IMFMediaType>> types = { videoType };
    audioCapture.AppendOutputTypes(types);
    std::vector<AudioTrackMetadata> audioTrackMetadata = audioCapture.GetTrackMetadata();

    wchar_t timestamp[64] = {};
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm localTime {};
    localtime_s(&localTime, &now);
    wcsftime(timestamp, 64, L"clip_%Y%m%d_%H%M%S.mp4", &localTime);

    const std::filesystem::path clipPath = std::filesystem::path(storagePath) / timestamp;
    std::thread(AsyncSaveWorker, ringBuffer.GetSnapshot(), types, audioTrackMetadata, clipPath.wstring(), exportEndHns).detach();
}

}

void RunCaptureSession(const Settings& settings) {
    DesktopCaptureTarget captureTarget;
    if (!TryCreateDesktopCaptureTarget(captureTarget)) {
        std::cerr << "DXGI: DuplicateOutput failed." << std::endl;
        return;
    }

    D3D11Context d3d = captureTarget.d3d;
    auto outputDuplication = captureTarget.duplication;
    DXGI_OUTDUPL_DESC duplicationDesc = captureTarget.duplicationDesc;

    auto dxgiDeviceManager = CreateDXGIDeviceManager(d3d.device.Get());
    if (!dxgiDeviceManager.deviceManager) {
        std::cerr << "DXGI: device manager creation failed." << std::endl;
        return;
    }

    ComPtr<IMFTransform> videoEncoder;
    HRESULT hr = InitializeHardwareEncoder(
        dxgiDeviceManager.deviceManager.Get(),
        videoEncoder,
        duplicationDesc.ModeDesc.Width,
        duplicationDesc.ModeDesc.Height,
        BuildVideoEncoderSettings(settings));
    if (FAILED(hr) || !videoEncoder) {
        std::cerr << "VIDEO: encoder init failed 0x" << std::hex << hr << std::dec << std::endl;
        return;
    }

    ComPtr<ID3D11Texture2D> copyTexture;
    if (!TryCreateCopyTexture(d3d.device.Get(), duplicationDesc.ModeDesc.Width, duplicationDesc.ModeDesc.Height, copyTexture)) {
        return;
    }
    ClearTexture(d3d.device.Get(), d3d.deviceContext.Get(), copyTexture.Get());

    RingBuffer ringBuffer(static_cast<LONGLONG>(settings.bufferDuration) * HnsPerSecond);

    MultiAppAudioCapture audioCapture;
    hr = audioCapture.Start(settings);
    if (FAILED(hr)) {
        std::cerr << "AUDIO: multi-app capture disabled 0x" << std::hex << hr << std::dec << std::endl;
    }

    ComPtr<IMFMediaEventGenerator> videoEventGen;
    videoEncoder.As(&videoEventGen);

    if (!BeginVideoEncoderStreaming(videoEncoder.Get())) {
        std::cerr << "VIDEO: encoder streaming start failed." << std::endl;
        audioCapture.Stop();
        return;
    }

    LARGE_INTEGER startQpc{};
    QueryPerformanceCounter(&startQpc);
    const LONGLONG masterStartHns = RawQpcToHns(startQpc.QuadPart);
    audioCapture.SetStartTime(static_cast<UINT64>(startQpc.QuadPart));

    HANDLE frameTimer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    UINT64 nextFrameIndex = 0;
    int framesProduced = 0;
    bool pendingSave = false;
    LONGLONG pendingSaveEndHns = 0;
    LONGLONG pendingSaveReadyHns = 0;
    bool duplicationLost = false;
    LONGLONG nextDuplicationReconnectHns = 0;
    std::cout << "Capture Fern active (" << settings.fps << " FPS). F9 pour sauvegarder." << std::endl;

    while (running) {
        audioCapture.Pump(ringBuffer);
        DrainVideoEncoder(videoEncoder.Get(), videoEventGen.Get(), ringBuffer, framesProduced, settings.fps);

        LONGLONG nowHns = CurrentQpcHns();
        int submittedThisLoop = 0;
        while (submittedThisLoop < kMaxCatchUpFramesPerLoop) {
            const LONGLONG relativeDueHns = FrameBoundaryHns(nextFrameIndex, settings.fps);
            const LONGLONG absoluteDueHns = masterStartHns + relativeDueHns;
            if (nowHns < absoluteDueHns) break;

            DesktopFrameResult frameResult = TryUpdateDesktopFrame(outputDuplication.Get(), d3d.deviceContext.Get(), copyTexture.Get());
            if (frameResult == DesktopFrameResult::AccessLost) {
                if (!duplicationLost) {
                    std::cerr << "VIDEO: duplication access lost; reconnecting." << std::endl;
                    ClearTexture(d3d.device.Get(), d3d.deviceContext.Get(), copyTexture.Get());
                    duplicationLost = true;
                    nextDuplicationReconnectHns = 0;
                }

                if (nowHns >= nextDuplicationReconnectHns) {
                    ComPtr<IDXGIOutputDuplication> restoredDuplication;
                    DXGI_OUTDUPL_DESC restoredDesc{};
                    std::wstring restoredOutputName;

                    if (TryCreateDuplicationOnCurrentDevice(
                            d3d.device.Get(),
                            captureTarget.outputName,
                            restoredDuplication,
                            restoredDesc,
                            restoredOutputName)) {
                        if (SameDesktopDimensions(duplicationDesc, restoredDesc)) {
                            outputDuplication = restoredDuplication;
                            duplicationDesc = restoredDesc;
                            captureTarget.duplication = outputDuplication;
                            captureTarget.duplicationDesc = duplicationDesc;
                            captureTarget.outputName = restoredOutputName;
                            frameResult = TryUpdateDesktopFrame(outputDuplication.Get(), d3d.deviceContext.Get(), copyTexture.Get());
                            if (frameResult == DesktopFrameResult::AccessLost) {
                                ClearTexture(d3d.device.Get(), d3d.deviceContext.Get(), copyTexture.Get());
                                duplicationLost = true;
                            } else {
                                duplicationLost = false;
                                std::cerr << "VIDEO: desktop duplication restored." << std::endl;
                            }
                        } else {
                            std::cerr << "VIDEO: display mode changed; rebuilding video pipeline." << std::endl;
                            if (RebuildVideoPipeline(
                                    settings,
                                    captureTarget,
                                    dxgiDeviceManager,
                                    videoEncoder,
                                    videoEventGen,
                                    copyTexture)) {
                                d3d = captureTarget.d3d;
                                outputDuplication = captureTarget.duplication;
                                duplicationDesc = captureTarget.duplicationDesc;
                                ringBuffer.Clear();
                                framesProduced = 0;
                                pendingSave = false;
                                std::cerr << "VIDEO: video pipeline rebuilt; replay buffer restarted." << std::endl;
                                frameResult = TryUpdateDesktopFrame(outputDuplication.Get(), d3d.deviceContext.Get(), copyTexture.Get());
                                if (frameResult == DesktopFrameResult::AccessLost) {
                                    ClearTexture(d3d.device.Get(), d3d.deviceContext.Get(), copyTexture.Get());
                                    duplicationLost = true;
                                } else {
                                    duplicationLost = false;
                                }
                            }
                        }
                    }

                    if (duplicationLost) {
                        nextDuplicationReconnectHns = nowHns + kDuplicationReconnectIntervalHns;
                    }
                }
            } else if (duplicationLost && (frameResult == DesktopFrameResult::Updated || frameResult == DesktopFrameResult::Unchanged)) {
                duplicationLost = false;
                std::cerr << "VIDEO: desktop duplication restored." << std::endl;
            }

            const LONGLONG nextBoundary = FrameBoundaryHns(nextFrameIndex + 1, settings.fps);
            const LONGLONG duration = std::max<LONGLONG>(1, nextBoundary - relativeDueHns);
            hr = PushFrameToEncoder(videoEncoder.Get(), copyTexture.Get(), relativeDueHns, duration);
            if (hr == MF_E_NOTACCEPTING) break;

            if (FAILED(hr)) {
                std::cerr << "VIDEO: ProcessInput failed 0x" << std::hex << hr << std::dec << std::endl;
            }

            ++nextFrameIndex;
            ++submittedThisLoop;
        }

        DrainVideoEncoder(videoEncoder.Get(), videoEventGen.Get(), ringBuffer, framesProduced, settings.fps);

        if (triggerSave.exchange(false)) {
            ShowCaptureFeedback();
            pendingSave = true;
            pendingSaveEndHns = FrameBoundaryHns(nextFrameIndex, settings.fps);
            pendingSaveReadyHns = CurrentQpcHns() + kSaveAudioSettleHns;
        }

        if (pendingSave && CurrentQpcHns() >= pendingSaveReadyHns) {
            audioCapture.Pump(ringBuffer);
            DrainVideoEncoder(videoEncoder.Get(), videoEventGen.Get(), ringBuffer, framesProduced, settings.fps);
            StartAsyncSave(ringBuffer, videoEncoder.Get(), audioCapture, settings.storagePath, pendingSaveEndHns);
            pendingSave = false;
        }

        const LONGLONG nextDueHns = masterStartHns + FrameBoundaryHns(nextFrameIndex, settings.fps);
        WaitBriefly(frameTimer, CurrentQpcHns(), nextDueHns);
    }

    if (frameTimer) CloseHandle(frameTimer);
    audioCapture.Stop();
}

}
