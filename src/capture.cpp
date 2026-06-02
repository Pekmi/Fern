#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>

#include "../include/fern/capture.h"


#pragma region getters

D3D11Context GetD3D11Device() {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* deviceContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    //cree D3D11 device
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Use default adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Use hardware driver
        nullptr,                    // No software rasterizer
        creationFlags,              // Flags
        nullptr,                    // Feature levels array
        0,                          // Number of feature levels
        D3D11_SDK_VERSION,          // SDK version
        &device,                    // Output device pointer
        &featureLevel,              // Output feature level
        &deviceContext              // Output context pointer (not needed here)
    );

    if (SUCCEEDED(hr) && device) {
        ID3D11Multithread* pMultithread = nullptr;
        hr = device->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread);
        if (SUCCEEDED(hr) && pMultithread) {
            pMultithread->SetMultithreadProtected(TRUE);
            pMultithread->Release();
        }
    }

    D3D11Context result = {device, deviceContext};
    return result;
}

//factory
IDXGIFactory1* getFactory1() {
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    return factory;
}

//adapters
std::vector<IDXGIAdapter1*> getAdapters1(IDXGIFactory1* factory) {
    int index = 0;
    std::vector<IDXGIAdapter1*> adapters;
    IDXGIAdapter1* adapter = nullptr;
    while (factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND) {
        adapters.push_back(adapter);
        index++;
    }
    return adapters;
}

//outputs
std::vector<IDXGIOutput1*> getOutputs1(IDXGIAdapter1* adapter) {
    int index = 0;
    std::vector<IDXGIOutput1*> outputs;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    while (adapter->EnumOutputs(index, &output) != DXGI_ERROR_NOT_FOUND) {
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        outputs.push_back(output1);
        index++;
    }
    return outputs;
}

//outputduplication
IDXGIOutputDuplication* getOutputDuplication(ID3D11Device* device, IDXGIOutput1* output1) {
    IDXGIOutputDuplication* outputDuplication = nullptr;
    output1->DuplicateOutput(device, &outputDuplication);
    return outputDuplication;
}


#pragma region capture 

//resource (image)
IDXGIResource* getResource(IDXGIOutputDuplication* outputDuplication, UINT timeout) {
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* resource = nullptr;
    outputDuplication->AcquireNextFrame(timeout, &frameInfo, &resource);
    return resource;
}


#pragma region decodage

//converti la ressource en texture
ID3D11Texture2D* resourceToTexture(IDXGIResource* resource) {
    ID3D11Texture2D* texture = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
    return texture;
}

//crée une texture relai lisable pour cpu (staging texture)
ID3D11Texture2D* createStagingTexture(ID3D11Device* device, ID3D11Texture2D* texture) {
    ID3D11Texture2D* stagingTexture;

    UINT width = 0;
    UINT height = 0;
    D3D11_TEXTURE2D_DESC desc; 
    texture->GetDesc(&desc);
    width = desc.Width;
    height = desc.Height;

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;

    device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);

    return stagingTexture;
}

//lecture pixels
void MapAndPrintPixel(ID3D11DeviceContext* deviceContext, ID3D11Texture2D* stagingTexture) {
    ID3D11Texture2D* capturedTexture = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped;
    
    deviceContext->CopyResource(stagingTexture, capturedTexture);
    deviceContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    deviceContext->Unmap(stagingTexture, 0);
}
