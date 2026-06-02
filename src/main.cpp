#include <iostream>
#include <vector>

#include "../include/fern/capture.h"
#include "../include/fern/encoder.h"

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

    // std::cout << "Attrape l'image" << std::endl;
    // IDXGIResource* resource = getResource(outputDuplication);
    
    std::cout << "== TRAITEMENT ==" << std::endl;

    // ID3D11Texture2D* texture = resourceToTexture(resource);
    // ID3D11Texture2D* stagingTexture = createStagingTexture(device, texture);
    // MapAndPrintPixel(deviceContext, stagingTexture);

    HRESULT hr = InitializeMediaFoundation();
    if (SUCCEEDED(hr)) { std::cout << "Media Foundation ok" << std::endl; }
    else { std::cerr << "Media Foundation erreur : " << hr << std::endl; }
    DXGIDeviceManagerAndUInt dxgiDeviceManager = CreateDXGIDeviceManager(device);
    IMFSinkWriter* sinkWriter = nullptr;
    DWORD streamIndex = 0;
    hr = CreateSinkWriter(L"output.mp4", dxgiDeviceManager.deviceManager, &sinkWriter, &streamIndex, desc.ModeDesc.Width, desc.ModeDesc.Height);
    if (FAILED(hr)) {
        std::cerr << "erreur creation sink writer : " << hr << std::endl;
        ShutdownMediaFoundation();
        return -1; 
    } 
    else {
        LONGLONG hnsTimestamp = 0;
        UINT fps = 60;
        std::cout << "Enregistrement" << std::endl;
        for (int i = 0; i < 600; ++i) {
            IDXGIResource* resource = getResource(outputDuplication);
            if (resource == nullptr) { continue; }
            ID3D11Texture2D* texture = resourceToTexture(resource);
            // check texture description
            D3D11_TEXTURE2D_DESC textureDesc;
            texture->GetDesc(&textureDesc);
            std::cout << "flags : " << textureDesc.BindFlags << ", " << textureDesc.MiscFlags << std::endl;
            hr = EncodeFrame(sinkWriter, streamIndex, texture, hnsTimestamp);
            if (FAILED(hr)) {
                std::cerr << "erreur encodage frame : " << hr << std::endl;
                break;
            }
            hnsTimestamp += 10000000 / fps;
            resource->Release();
            texture->Release();
            outputDuplication->ReleaseFrame();
        }
    }

    std::cout << "== CLEANUP ==" << std::endl;
    sinkWriter->Finalize();
    sinkWriter->Release();
    dxgiDeviceManager.deviceManager->Release();
    ShutdownMediaFoundation();
}