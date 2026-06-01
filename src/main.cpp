#include <iostream>
#include <vector>

#include "../include/fern/capture.h"

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

    std::cout << "Attrape l'image" << std::endl;
    IDXGIResource* resource = getResource(outputDuplication);
    
    std::cout << "== DECODAGE ==" << std::endl;

    ID3D11Texture2D* texture = resourceToTexture(resource);
    ID3D11Texture2D* stagingTexture = createStagingTexture(device, texture);
    MapAndPrintPixel(deviceContext, stagingTexture);
}