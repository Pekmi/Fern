#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/capture_session.h"

#include "../include/fern/buffer.h"
#include "../include/fern/capture.h"
#include "../include/fern/capture_feedback.h"
#include "../include/fern/clock.h"
#include "../include/fern/encoder.h"
#include "../include/fern/isolatedAudioCapture.h"
#include "../include/fern/ipc_server.h"
#include "../include/fern/replay_export.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mferror.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace fern {
namespace {

constexpr int kMaxCatchUpFramesPerLoop = 4;
constexpr LONGLONG kSaveAudioSettleHns = 2500000LL;

bool TryUpdateDesktopFrame(IDXGIOutputDuplication* duplication, ID3D11DeviceContext* context, ID3D11Texture2D* target) {
    if (!duplication || !context || !target) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::cerr << "VIDEO: duplication access lost, keeping last frame." << std::endl;
        }
        return false;
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
    return copied;
}

void ClearTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    if (!device || !context || !texture) return;

    ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &rtv)) && rtv) {
        const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearRenderTargetView(rtv.Get(), black);
    }
}

void DrainAudioEncoder(IMFTransform* encoder, RingBuffer& ringBuffer) {
    if (!encoder) return;

    for (;;) {
        ComPtr<IMFSample> output;
        HRESULT hr = PullSampleFromEncoder(encoder, output);
        if (hr != S_OK || !output) break;
        ringBuffer.AddSample(output.Get(), 1);
    }
}

void PumpAudio(IsolatedAudioCapture& audioCapture, IMFTransform* encoder, RingBuffer& ringBuffer) {
    if (!encoder) return;

    for (int i = 0; i < 8; ++i) {
        ComPtr<IMFSample> input;
        HRESULT hr = audioCapture.GetAudioSample(input);
        if (hr != S_OK || !input) break;

        hr = PushAudioToEncoder(encoder, input.Get());
        if (hr == MF_E_NOTACCEPTING) break;
        if (FAILED(hr)) {
            std::cerr << "AUDIO: ProcessInput failed 0x" << std::hex << hr << std::dec << std::endl;
            break;
        }

        DrainAudioEncoder(encoder, ringBuffer);
    }
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
    IMFTransform* audioEncoder,
    const std::wstring& storagePath,
    LONGLONG exportEndHns) {
    ComPtr<IMFMediaType> videoType;
    ComPtr<IMFMediaType> audioType;
    videoEncoder->GetOutputCurrentType(0, &videoType);

    std::vector<ComPtr<IMFMediaType>> types = { videoType };
    if (audioEncoder && SUCCEEDED(audioEncoder->GetOutputCurrentType(0, &audioType)) && audioType) {
        types.push_back(audioType);
    }

    wchar_t timestamp[64] = {};
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm localTime {};
    localtime_s(&localTime, &now);
    wcsftime(timestamp, 64, L"clip_%Y%m%d_%H%M%S.mp4", &localTime);

    const std::filesystem::path clipPath = std::filesystem::path(storagePath) / timestamp;
    std::thread(AsyncSaveWorker, ringBuffer.GetSnapshot(), types, clipPath.wstring(), exportEndHns).detach();
}

}

void RunCaptureSession(const Settings& settings) {
    D3D11Context d3d = GetD3D11Device();
    if (!d3d.device || !d3d.deviceContext) {
        std::cerr << "D3D11: device creation failed." << std::endl;
        return;
    }

    auto factory = getFactory1();
    auto adapters = getAdapters1(factory.Get());
    if (adapters.empty()) {
        std::cerr << "DXGI: no adapter found." << std::endl;
        return;
    }

    auto outputs = getOutputs1(adapters[0].Get());
    if (outputs.empty()) {
        std::cerr << "DXGI: no output found." << std::endl;
        return;
    }

    auto outputDuplication = getOutputDuplication(d3d.device.Get(), outputs[0].Get());
    if (!outputDuplication) {
        std::cerr << "DXGI: DuplicateOutput failed." << std::endl;
        return;
    }

    DXGI_OUTDUPL_DESC duplicationDesc{};
    outputDuplication->GetDesc(&duplicationDesc);

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
        settings.fps,
        settings.bitrate);
    if (FAILED(hr) || !videoEncoder) {
        std::cerr << "VIDEO: encoder init failed 0x" << std::hex << hr << std::dec << std::endl;
        return;
    }

    ComPtr<ID3D11Texture2D> copyTexture;
    D3D11_TEXTURE2D_DESC copyDesc = {};
    copyDesc.Width = duplicationDesc.ModeDesc.Width;
    copyDesc.Height = duplicationDesc.ModeDesc.Height;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = d3d.device->CreateTexture2D(&copyDesc, nullptr, &copyTexture);
    if (FAILED(hr) || !copyTexture) {
        std::cerr << "VIDEO: copy texture creation failed 0x" << std::hex << hr << std::dec << std::endl;
        return;
    }
    ClearTexture(d3d.device.Get(), d3d.deviceContext.Get(), copyTexture.Get());

    RingBuffer ringBuffer(static_cast<LONGLONG>(settings.bufferDuration) * HnsPerSecond);

    IsolatedAudioCapture audioCapture(0);
    ComPtr<IMFTransform> audioEncoder;
    if (SUCCEEDED(audioCapture.Start())) {
        hr = InitializeAudioEncoder(audioEncoder, audioCapture.GetFormat());
        if (FAILED(hr)) {
            std::cerr << "AUDIO: encoder init failed 0x" << std::hex << hr << std::dec << std::endl;
            audioEncoder.Reset();
        }
    } else {
        std::cerr << "AUDIO: capture disabled." << std::endl;
    }

    ComPtr<IMFMediaEventGenerator> videoEventGen;
    videoEncoder.As(&videoEventGen);

    videoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    videoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (audioEncoder) {
        audioEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        audioEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
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
    std::cout << "Capture Fern active (" << settings.fps << " FPS). F9 pour sauvegarder." << std::endl;

    while (running) {
        PumpAudio(audioCapture, audioEncoder.Get(), ringBuffer);
        DrainVideoEncoder(videoEncoder.Get(), videoEventGen.Get(), ringBuffer, framesProduced, settings.fps);

        LONGLONG nowHns = CurrentQpcHns();
        int submittedThisLoop = 0;
        while (submittedThisLoop < kMaxCatchUpFramesPerLoop) {
            const LONGLONG relativeDueHns = FrameBoundaryHns(nextFrameIndex, settings.fps);
            const LONGLONG absoluteDueHns = masterStartHns + relativeDueHns;
            if (nowHns < absoluteDueHns) break;

            TryUpdateDesktopFrame(outputDuplication.Get(), d3d.deviceContext.Get(), copyTexture.Get());

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
            PumpAudio(audioCapture, audioEncoder.Get(), ringBuffer);
            DrainVideoEncoder(videoEncoder.Get(), videoEventGen.Get(), ringBuffer, framesProduced, settings.fps);
            StartAsyncSave(ringBuffer, videoEncoder.Get(), audioEncoder.Get(), settings.storagePath, pendingSaveEndHns);
            pendingSave = false;
        }

        const LONGLONG nextDueHns = masterStartHns + FrameBoundaryHns(nextFrameIndex, settings.fps);
        WaitBriefly(frameTimer, CurrentQpcHns(), nextDueHns);
    }

    if (frameTimer) CloseHandle(frameTimer);
}

}
