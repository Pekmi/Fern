#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <synchapi.h>
#include <mferror.h>
#include <chrono>
#include <thread>
#include <tlhelp32.h>

#include "../include/fern/capture.h"
#include "../include/fern/encoder.h"
#include "../include/fern/buffer.h"
#include "../include/fern/ipc_server.h"
#include "../include/fern/hotkey.h"
#include "../include/fern/dump.h"
#include "../include/fern/isolatedAudioCapture.h"

#include <algorithm>

void AsyncSaveWorker(std::deque<StreamSample> samples, std::vector<ComPtr<IMFMediaType>> types, std::wstring filename) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) std::cerr << "SAVE ERROR: CoInitializeEx " << std::hex << hr << std::endl;
    
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) std::cerr << "SAVE ERROR: MFStartup " << std::hex << hr << std::endl;
    
    bool hasAudio = false;
    for (auto& ss : samples) if (ss.streamIndex == 1) { hasAudio = true; break; }
    
    //tri chronologique des samples
    std::vector<StreamSample> sortedSamples(samples.begin(), samples.end());
    std::sort(sortedSamples.begin(), sortedSamples.end(), [](const StreamSample& a, const StreamSample& b) {
        LONGLONG tA = 0, tB = 0;
        a.sample->GetSampleTime(&tA);
        b.sample->GetSampleTime(&tB);
        return tA < tB;
    });

    //filtrage, le fichier commence par une I-Frame Vidéo
    std::vector<StreamSample> validSamples;
    bool videoKeyframeFound = false;
    for (auto& ss : sortedSamples) {
        if (!videoKeyframeFound) {
            if (ss.streamIndex == 0) { //sample vidéo
                UINT32 isCleanPoint = 0;
                if (SUCCEEDED(ss.sample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint)) && isCleanPoint) {
                    videoKeyframeFound = true;
                    ss.sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
                    validSamples.push_back(ss);
                }
            }
            //on jette le reste
        } else {
            validSamples.push_back(ss);
        }
    }

    if (validSamples.empty()) {
        std::cerr << "SAVE ERROR: Aucune I-Frame video trouvee. Fichier annule." << std::endl;
    } else {
        ComPtr<IMFAttributes> pAttributes;
        MFCreateAttributes(&pAttributes, 1);
        pAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        ComPtr<IMFSinkWriter> pSinkWriter;
        DWORD vIdx = 0, aIdx = 0;
        
        hr = MFCreateSinkWriterFromURL(filename.c_str(), nullptr, pAttributes.Get(), &pSinkWriter);
        if (FAILED(hr)) {
            std::cerr << "SAVE ERROR: MFCreateSinkWriterFromURL " << std::hex << hr << std::endl;
        } else {
            hr = pSinkWriter->AddStream(types[0].Get(), &vIdx);
            if (FAILED(hr)) std::cerr << "SAVE ERROR: AddStream Vid " << std::hex << hr << std::endl;
            
            if (hasAudio && types.size() > 1) {
                hr = pSinkWriter->AddStream(types[1].Get(), &aIdx);
                if (FAILED(hr)) std::cerr << "SAVE ERROR: AddStream Aud " << std::hex << hr << std::endl;
            }
            
            hr = pSinkWriter->BeginWriting();
            if (FAILED(hr)) {
                std::cerr << "SAVE ERROR: BeginWriting " << std::hex << hr << std::endl;
            } else {
                LONGLONG offset = -1;
                int writtenVid = 0, writtenAud = 0;
                int totalSamples = validSamples.size();
                int processed = 0;
                
                for (auto& ss : validSamples) {
                    processed++;
                    if (processed % 50 == 0 || processed == totalSamples) {
                        std::cout << "\r>>> Sauvegarde en cours... " << (processed * 100 / totalSamples) << "% (" << processed << "/" << totalSamples << ")   " << std::flush;
                    }

                    if (ss.streamIndex == 1 && !hasAudio) continue;
                    
                    LONGLONG time = 0; ss.sample->GetSampleTime(&time);
                    if (offset == -1) offset = time;
                    
                    ComPtr<IMFSample> pOut; MFCreateSample(&pOut);
                    ss.sample->CopyAllItems(pOut.Get());
                    
                    //recalage temporel
                    LONGLONG newTime = time - offset;
                    if (newTime < 0) newTime = 0;
                    pOut->SetSampleTime(newTime);
                    
                    UINT64 dts = 0;
                    if (SUCCEEDED(pOut->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) {
                        LONGLONG newDts = (LONGLONG)dts - offset;
                        if (newDts < 0) newDts = 0;
                        pOut->SetUINT64(MFSampleExtension_DecodeTimestamp, newDts);
                    }
                    
                    LONGLONG dur = 0; if (SUCCEEDED(ss.sample->GetSampleDuration(&dur))) pOut->SetSampleDuration(dur);
                    
                    DWORD bCount = 0; ss.sample->GetBufferCount(&bCount);
                    for (DWORD i = 0; i < bCount; i++) {
                        ComPtr<IMFMediaBuffer> pB; ss.sample->GetBufferByIndex(i, &pB);
                        pOut->AddBuffer(pB.Get());
                    }
                    
                    hr = pSinkWriter->WriteSample(ss.streamIndex == 0 ? vIdx : aIdx, pOut.Get());
                    if (FAILED(hr)) {
                        std::cerr << "\nSAVE ERROR: WriteSample (Stream " << ss.streamIndex << ") 0x" << std::hex << hr << std::dec << std::endl;
                    } else {
                        if (ss.streamIndex == 0) writtenVid++; else writtenAud++;
                    }
                }
                std::cout << "\n>>> Samples ecrits - Video: " << writtenVid << ", Audio: " << writtenAud << std::endl;
                
                hr = pSinkWriter->Finalize();
                if (FAILED(hr)) std::cerr << "SAVE ERROR: Finalize " << std::hex << hr << std::endl;
            }
        }
    }

    std::wcout << L">>> Exportation terminee : " << filename << (hasAudio ? L" (Video+Audio)" : L" (Video seule)") << std::endl;
    MFShutdown();
    CoUninitialize();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    AllocConsole();
    FILE* fpOUT; freopen_s(&fpOUT, "CONOUT$", "w", stdout);
    FILE* fpERR; freopen_s(&fpERR, "CONOUT$", "w", stderr);
    FILE* fpIN;  freopen_s(&fpIN,  "CONIN$",  "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

    int targetFps = 60; int maxClipDurationSeconds = 30; LONGLONG tickIntervalHns = HNS_PER_SEC / targetFps;
    extern std::atomic<bool> running; extern std::atomic<bool> triggerSave;
    std::thread ipcThread(RunIpcServer, std::ref(running), std::ref(triggerSave)); ipcThread.detach();
    std::thread hotkeyThread(RunHotkeyListener, std::ref(running), std::ref(triggerSave)); hotkeyThread.detach();
    InitializeMediaFoundation();
    {
        D3D11Context structDevice = GetD3D11Device(); if (!structDevice.device) goto cleanup;
        auto outputDuplication = getOutputDuplication(structDevice.device.Get(), getOutputs1(getAdapters1(getFactory1().Get())[0].Get())[0].Get());
        if (!outputDuplication) goto cleanup;
        DXGI_OUTDUPL_DESC duplDesc; outputDuplication->GetDesc(&duplDesc);
        auto dxgiDeviceManager = CreateDXGIDeviceManager(structDevice.device.Get());

        //init audio + verif
        IsolatedAudioCapture audioCapture(0); 
        ComPtr<IMFTransform> pAudioEncoder;
        if (SUCCEEDED(audioCapture.Start())) {
            HRESULT hrAudio = InitializeAudioEncoder(pAudioEncoder, audioCapture.GetFormat());
            if (SUCCEEDED(hrAudio)) std::cout << "AUDIO: Encodeur AAC pret." << std::endl;
            else std::cerr << "AUDIO ERROR: Echec encodeur AAC (0x" << std::hex << hrAudio << ")" << std::endl;
        }

        ComPtr<IMFTransform> pVideoEncoder;
        InitializeHardwareEncoder(dxgiDeviceManager.deviceManager.Get(), pVideoEncoder, duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height);
        RingBuffer ringBuffer(maxClipDurationSeconds * HNS_PER_SEC);
        ComPtr<IMFMediaEventGenerator> pVideoEventGen; pVideoEncoder.As(&pVideoEventGen);
        ComPtr<ID3D11Texture2D> copyTexture;
        D3D11_TEXTURE2D_DESC copyDesc = { duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        structDevice.device->CreateTexture2D(&copyDesc, nullptr, &copyTexture);
        pVideoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        pVideoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (pAudioEncoder) { pAudioEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); pAudioEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0); }
        HANDLE hTimer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER liDueTime; liDueTime.QuadPart = -tickIntervalHns;
        SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);
        LARGE_INTEGER startQpc; QueryPerformanceCounter(&startQpc);
        audioCapture.SetStartTime(startQpc.QuadPart);
        LONGLONG hnsTimestamp = 0; int framesProduced = 0;
        std::cout << "Capture Fern active. F9 pour sauvegarder." << std::endl;

        while (running) {
            if (triggerSave.exchange(false)) {
                ComPtr<IMFMediaType> vt, at;
                pVideoEncoder->GetOutputCurrentType(0, &vt);
                std::vector<ComPtr<IMFMediaType>> types = { vt };
                if (pAudioEncoder) { pAudioEncoder->GetOutputCurrentType(0, &at); types.push_back(at); }
                auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                struct tm ti; localtime_s(&ti, &now); wchar_t ts[64]; wcsftime(ts, 64, L"clip_%Y%m%d_%H%M%S.mp4", &ti);
                std::thread(AsyncSaveWorker, ringBuffer.GetSnapshot(), types, std::wstring(ts)).detach();
            }
            if (pAudioEncoder) {
                ComPtr<IMFSample> asIn;
                if (audioCapture.GetAudioSample(asIn) == S_OK) {
                    HRESULT hrPush = PushAudioToEncoder(pAudioEncoder.Get(), asIn.Get());
                    if (FAILED(hrPush)) {
                        std::cerr << "PushAudio ERROR: 0x" << std::hex << hrPush << std::dec << std::endl;
                    }
                    ComPtr<IMFSample> asOut;
                    while (SUCCEEDED(PullSampleFromEncoder(pAudioEncoder.Get(), asOut)) && asOut) {
                        ringBuffer.AddSample(asOut.Get(), 1);
                        asOut.Reset();
                    }
                }
            }
            ComPtr<IMFMediaEvent> pEvent;
            HRESULT hr = pVideoEventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (FAILED(hr)) { std::this_thread::sleep_for(std::chrono::microseconds(500)); continue; }
            MediaEventType et; pEvent->GetType(&et);
            if (et == METransformNeedInput) {
                WaitForSingleObject(hTimer, INFINITE);
                liDueTime.QuadPart = -tickIntervalHns; SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);
                auto res = getResource(outputDuplication.Get());
                if (res) {
                    auto tex = resourceToTexture(res.Get());
                    if (tex) structDevice.deviceContext->CopyResource(copyTexture.Get(), tex.Get());
                    outputDuplication->ReleaseFrame();
                } 
                if (SUCCEEDED(PushFrameToEncoder(pVideoEncoder.Get(), copyTexture.Get(), hnsTimestamp))) hnsTimestamp += tickIntervalHns;
            }
            else if (et == METransformHaveOutput) {
                ComPtr<IMFSample> vs;
                if (SUCCEEDED(PullSampleFromEncoder(pVideoEncoder.Get(), vs)) && vs) {
                    framesProduced++; ringBuffer.AddSample(vs.Get(), 0);
                    if (framesProduced % 60 == 0) std::cout << ">>> Status | Frames: " << framesProduced << " | Buffer total: " << ringBuffer.GetSampleCount() << std::endl;
                }
            }
            else if (et == METransformDrainComplete) break;
        }
        if (hTimer) CloseHandle(hTimer);
    }
cleanup:
    ShutdownMediaFoundation(); return 0;
}
