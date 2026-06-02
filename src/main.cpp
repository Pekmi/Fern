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

        // 1. Créer la texture de copie UNE SEULE FOIS avant la boucle
        ID3D11Texture2D* copyTexture = nullptr;
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

        for (int i = 0; i < 600; ++i) {
            IDXGIResource* resource = getResource(outputDuplication);
            if (resource == nullptr) {
                hnsTimestamp += 10000000 / fps;
                continue; 
            }
            ID3D11Texture2D* texture = resourceToTexture(resource);
            
            // 2. Copier et encoder
            if (copyTexture) {
                deviceContext->CopyResource(copyTexture, texture);
                hr = EncodeFrame(sinkWriter, streamIndex, copyTexture, hnsTimestamp);
                
                if (FAILED(hr)) {
                    std::cerr << "erreur encodage frame : " << hr << std::endl;
                    texture->Release();
                    resource->Release();
                    break;
                }
            }
            
            hnsTimestamp += 10000000 / fps;
            texture->Release(); // 3. Libérer la texture capturée
            resource->Release();
            outputDuplication->ReleaseFrame(); // 4. Relâcher le verrou immédiatement
        }
        if (copyTexture) copyTexture->Release(); // 5. Nettoyage final
    }

    std::cout << "== CLEANUP ==" << std::endl;
    sinkWriter->Finalize();
    sinkWriter->Release();
    dxgiDeviceManager.deviceManager->Release();
    ShutdownMediaFoundation();
}