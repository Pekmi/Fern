#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <synchapi.h>
#include <mferror.h>
#include <chrono>

#include "../include/fern/capture.h"
#include "../include/fern/encoder.h"
#include "../include/fern/buffer.h"

int main() {
    std::cout << "== SETUP ==" << std::endl;

    std::cout << "Cree le device" << std::endl;
    D3D11Context structDevice = GetD3D11Device();
    ID3D11Device* device = structDevice.device;
    ID3D11DeviceContext* deviceContext = structDevice.deviceContext;
    
    std::cout << "Attrape la factory" << std::endl;
    IDXGIFactory1* factory = getFactory1();

    std::cout << "Attrape les adapters" << std::endl;
    std::vector<IDXGIAdapter1*> adapters = getAdapters1(factory);
    for (size_t i = 0; i < adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[i]->GetDesc1(&desc);
        std::wcout << "> Adapter " << i << ": " << desc.Description << std::endl;
    }

    std::cout << "Attrape les outputs" << std::endl;
    std::vector<IDXGIOutput1*> outputs = getOutputs1(adapters[0]);
    for (size_t i = 0; i < outputs.size(); ++i) {
        DXGI_OUTPUT_DESC desc;
        outputs[i]->GetDesc(&desc);
        std::wcout << "> Sortie " << i << ": " << desc.DeviceName << std::endl;
    }

    std::cout << "Attrape l'output duplication" << std::endl;
    IDXGIOutputDuplication* outputDuplication = getOutputDuplication(device, outputs[0]);
    DXGI_OUTDUPL_DESC desc;
    outputDuplication->GetDesc(&desc);
    std::wcout << "> Resolution sortie dupliquee : " << desc.ModeDesc.Width << "x" << desc.ModeDesc.Height << std::endl;

    std::cout << "== TRAITEMENT ==" << std::endl;

    HRESULT hr = InitializeMediaFoundation();
    if (SUCCEEDED(hr)) { std::cout << "Media Foundation ok" << std::endl; }
    else { std::cerr << "Media Foundation erreur : " << hr << std::endl; }

    DXGIDeviceManagerAndUInt dxgiDeviceManager = CreateDXGIDeviceManager(device);
    IMFTransform* pEncoder = nullptr;
    hr = InitializeHardwareEncoder(dxgiDeviceManager.deviceManager, &pEncoder, desc.ModeDesc.Width, desc.ModeDesc.Height);
    
    if (FAILED(hr)) {
        std::cerr << "erreur initialisation encodeur hardware : " << hr << std::endl;
        ShutdownMediaFoundation();
        return -1; 
    } 
    else {
        std::cout << "Encodeur hardware initialise avec succes" << std::endl;
        
        //cree tampon circulaire (30s)
        RingBuffer ringBuffer(30LL * HNS_PER_SEC);

        //gen d'events de l'encodeur
        ComPtr<IMFMediaEventGenerator> pEventGen;
        hr = pEncoder->QueryInterface(IID_PPV_ARGS(&pEventGen));
        if (FAILED(hr)) return -1;

        //crée texture de copie (Zero-Copy source)
        ComPtr<ID3D11Texture2D> copyTexture;
        D3D11_TEXTURE2D_DESC copyDesc = {};
        copyDesc.Width = desc.ModeDesc.Width;
        copyDesc.Height = desc.ModeDesc.Height;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&copyDesc, nullptr, &copyTexture);

        //démarre le flux
        pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        // Initialisation du timer pour 60 FPS
        HANDLE hTimer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!hTimer) {
            std::cerr << "Erreur creation timer : " << GetLastError() << std::endl;
            return -1;
        }

        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = -TICK_INTERVAL_HNS; // Démarre immédiatement (relatif)
        SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);

        std::cout << "Capture en cours (600 frames)..." << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        LONGLONG hnsTimestamp = 0;
        int framesProduced = 0;

        for (int i = 0; i < 600; ) {
            ComPtr<IMFMediaEvent> pEvent;
            hr = pEventGen->GetEvent(0, &pEvent);
            if (FAILED(hr)) break;

            MediaEventType eventType;
            pEvent->GetType(&eventType);

            if (eventType == METransformNeedInput) {
                // Attente du prochain tick
                WaitForSingleObject(hTimer, INFINITE);

                // Re-programmer le timer pour le prochain tick
                liDueTime.QuadPart = -TICK_INTERVAL_HNS;
                SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);

                // Attrape une nouvelle image
                IDXGIResource* resource = getResource(outputDuplication);
                
                if (resource) {
                    ComPtr<ID3D11Texture2D> texture = resourceToTexture(resource);
                    deviceContext->CopyResource(copyTexture.Get(), texture.Get());
                    resource->Release();
                    outputDuplication->ReleaseFrame();
                } 
                
                // envoie toujours copyTexture (nouvelle ou ancienne)
                if (SUCCEEDED(PushFrameToEncoder(pEncoder, copyTexture.Get(), hnsTimestamp))) {
                    hnsTimestamp += TICK_INTERVAL_HNS;
                    i++;
                }
            }
            else if (eventType == METransformHaveOutput) {
                ComPtr<IMFSample> pSample;
                if (SUCCEEDED(PullSampleFromEncoder(pEncoder, &pSample))) {
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
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        std::cout << "Capture terminee. RAM : " << ringBuffer.GetSampleCount() << " samples." << std::endl;
        std::cout << "Performance : " << elapsed.count() << "s pour 600 frames (cible 10s)." << std::endl;
        std::cout << "FPS Moyen : " << 600.0 / elapsed.count() << std::endl;

        //garde le clip
        ComPtr<IMFMediaType> pCurrentType;
        if (SUCCEEDED(pEncoder->GetOutputCurrentType(0, &pCurrentType))) {
            ringBuffer.SaveToFile(L"clip.mp4", pCurrentType.Get());
        }

        CloseHandle(hTimer);
    }

    std::cout << "== CLEANUP ==" << std::endl;
    if (pEncoder) pEncoder->Release();
    if (dxgiDeviceManager.deviceManager) dxgiDeviceManager.deviceManager->Release();
    
    if (outputDuplication) outputDuplication->Release();
    for (auto output : outputs) if (output) output->Release();
    for (auto adapter : adapters) if (adapter) adapter->Release();
    if (factory) factory->Release();

    if (deviceContext) deviceContext->Release();
    if (device) device->Release();

    ShutdownMediaFoundation();
}