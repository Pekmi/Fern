#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <synchapi.h>
#include <mferror.h>
#include <chrono>
#include <thread>

#include "../include/fern/capture.h"
#include "../include/fern/encoder.h"
#include "../include/fern/buffer.h"
#include "../include/fern/ipc_server.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {

    //console
    AllocConsole();
    FILE* fpOUT;
    FILE* fpERR;
    FILE* fpIN;
    freopen_s(&fpOUT, "CONOUT$", "w", stdout);
    freopen_s(&fpERR, "CONOUT$", "w", stderr);
    freopen_s(&fpIN, "CONIN$", "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

    //init serveur IPC
    extern std::atomic<bool> running;
    extern std::atomic<bool> triggerSave;
    std::thread ipcThread(RunIpcServer, std::ref(running), std::ref(triggerSave));
    ipcThread.detach();

    HRESULT hr = InitializeMediaFoundation();
    if (FAILED(hr)) {
        std::cerr << "Media Foundation erreur d'initialisation : " << hr << std::endl;
        return -1;
    }

    HANDLE hTimer = NULL;
    
    // Bloc de portée pour assurer la destruction des ComPtr avant ShutdownMediaFoundation()
    {
        std::cout << "== SETUP ==" << std::endl;

        std::cout << "Cree le device" << std::endl;
        D3D11Context structDevice = GetD3D11Device();
        ComPtr<ID3D11Device> device = structDevice.device;
        ComPtr<ID3D11DeviceContext> deviceContext = structDevice.deviceContext;
        
        if (!device || !deviceContext) {
            std::cerr << "Erreur creation device D3D11" << std::endl;
            goto cleanup;
        }

        std::cout << "Attrape la factory" << std::endl;
        ComPtr<IDXGIFactory1> factory = getFactory1();
        if (!factory) {
            std::cerr << "Erreur acquisition DXGI Factory" << std::endl;
            goto cleanup;
        }

        std::cout << "Attrape les adapters" << std::endl;
        std::vector<ComPtr<IDXGIAdapter1>> adapters = getAdapters1(factory.Get());
        if (adapters.empty()) {
            std::cerr << "Aucun adapter trouve" << std::endl;
            goto cleanup;
        }
        for (size_t i = 0; i < adapters.size(); ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapters[i]->GetDesc1(&desc);
            std::wcout << "> Adapter " << i << ": " << desc.Description << std::endl;
        }

        std::cout << "Attrape les outputs" << std::endl;
        std::vector<ComPtr<IDXGIOutput1>> outputs = getOutputs1(adapters[0].Get());
        if (outputs.empty()) {
            std::cerr << "Aucune sortie trouvee sur le premier adapter" << std::endl;
            goto cleanup;
        }
        for (size_t i = 0; i < outputs.size(); ++i) {
            DXGI_OUTPUT_DESC desc;
            outputs[i]->GetDesc(&desc);
            std::wcout << "> Sortie " << i << ": " << desc.DeviceName << std::endl;
        }

        std::cout << "Attrape l'output duplication" << std::endl;
        ComPtr<IDXGIOutputDuplication> outputDuplication = getOutputDuplication(device.Get(), outputs[0].Get());
        if (!outputDuplication) {
            std::cerr << "Erreur creation output duplication" << std::endl;
            goto cleanup;
        }

        DXGI_OUTDUPL_DESC duplDesc;
        outputDuplication->GetDesc(&duplDesc);
        std::wcout << "> Resolution sortie dupliquee : " << duplDesc.ModeDesc.Width << "x" << duplDesc.ModeDesc.Height << std::endl;

        std::cout << "== TRAITEMENT ==" << std::endl;

        DXGIDeviceManagerAndUInt dxgiDeviceManager = CreateDXGIDeviceManager(device.Get());
        ComPtr<IMFDXGIDeviceManager> spDeviceManager = dxgiDeviceManager.deviceManager;
        
        ComPtr<IMFTransform> pEncoder;
        hr = InitializeHardwareEncoder(spDeviceManager.Get(), pEncoder, duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height);
        
        if (FAILED(hr)) {
            std::cerr << "erreur initialisation encodeur hardware : " << hr << std::endl;
            goto cleanup;
        } 
        
        std::cout << "Encodeur hardware initialise avec succes" << std::endl;
        
        {//scope pour libérer les ressources avant shutdown

            //cree tampon circulaire (30s)
            RingBuffer ringBuffer(30LL * HNS_PER_SEC);

            //gen d'events de l'encodeur
            ComPtr<IMFMediaEventGenerator> pEventGen;
            hr = pEncoder.As(&pEventGen);
            if (FAILED(hr)) goto cleanup;

            //crée texture de copie (Zero-Copy source)
            ComPtr<ID3D11Texture2D> copyTexture;
            D3D11_TEXTURE2D_DESC copyDesc = {};
            copyDesc.Width = duplDesc.ModeDesc.Width;
            copyDesc.Height = duplDesc.ModeDesc.Height;
            copyDesc.MipLevels = 1;
            copyDesc.ArraySize = 1;
            copyDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            copyDesc.SampleDesc.Count = 1;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = device->CreateTexture2D(&copyDesc, nullptr, &copyTexture);
            if (FAILED(hr)) goto cleanup;

            //démarre le flux
            pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

            // Initialisation du timer pour 60 FPS
            hTimer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!hTimer) {
                std::cerr << "Erreur creation timer : " << GetLastError() << std::endl;
                goto cleanup;
            }

            LARGE_INTEGER liDueTime;
            liDueTime.QuadPart = -TICK_INTERVAL_HNS; // Démarre immédiatement (relatif)
            SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);

            std::cout << "Capture en cours (600 frames)..." << std::endl;
            auto start = std::chrono::high_resolution_clock::now();
            LONGLONG hnsTimestamp = 0;
            int framesProduced = 0;

            // for (int i = 0; i < 600; ) {
            int i = 0;
            while (running) {

                if (triggerSave.exchange(false)) {
                    std::cout << ">>> Trigger sauvegarde demandee IPC" << std::endl;
                    ComPtr<IMFMediaType> pCurrentType;
                    if (SUCCEEDED(pEncoder->GetOutputCurrentType(0, &pCurrentType))) {
                        ringBuffer.SaveToFile(L"clip_triggered.mp4", pCurrentType.Get());
                    }
                }
                
                ComPtr<IMFMediaEvent> pEvent;
                hr = pEventGen->GetEvent(0, &pEvent);
                if (FAILED(hr)) {
                    // std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                MediaEventType eventType;
                pEvent->GetType(&eventType);

                if (eventType == METransformNeedInput) {
                    WaitForSingleObject(hTimer, INFINITE);
                    liDueTime.QuadPart = -TICK_INTERVAL_HNS;
                    SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);

                    ComPtr<IDXGIResource> resource = getResource(outputDuplication.Get());
                    
                    if (resource) {
                        ComPtr<ID3D11Texture2D> texture = resourceToTexture(resource.Get());
                        if (texture) {
                            deviceContext->CopyResource(copyTexture.Get(), texture.Get());
                        }
                        outputDuplication->ReleaseFrame();
                    } 
                    
                    if (SUCCEEDED(PushFrameToEncoder(pEncoder.Get(), copyTexture.Get(), hnsTimestamp))) {
                        hnsTimestamp += TICK_INTERVAL_HNS;
                        i++;
                    }
                }
                else if (eventType == METransformHaveOutput) {
                    ComPtr<IMFSample> pSample;
                    if (SUCCEEDED(PullSampleFromEncoder(pEncoder.Get(), pSample))) {
                        framesProduced++;
                        ringBuffer.AddSample(pSample.Get());
                        if (framesProduced % 60 == 0) {
                            std::cout << ">>> RAM: " << ringBuffer.GetSampleCount() << " samples" << std::endl;
                        }
                    }
                }
                else if (eventType == METransformDrainComplete) {
                    break;
                }

                i++;
            }
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            std::cout << "Capture terminee. RAM : " << ringBuffer.GetSampleCount() << " samples." << std::endl;
            std::cout << "Performance : " << elapsed.count() << "s pour 600 frames (cible 10s)." << std::endl;
            std::cout << "FPS Moyen : " << 600.0 / elapsed.count() << std::endl;

            ComPtr<IMFMediaType> pCurrentType;
            if (SUCCEEDED(pEncoder->GetOutputCurrentType(0, &pCurrentType))) {
                ringBuffer.SaveToFile(L"clip.mp4", pCurrentType.Get());
            }

            hr = S_OK;
        }
    }

cleanup:
    std::cout << "== CLEANUP ==" << std::endl;
    if (hTimer) CloseHandle(hTimer);
    ShutdownMediaFoundation();

    return SUCCEEDED(hr) ? 0 : -1;
}
