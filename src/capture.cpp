#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>

#include "../include/fern/capture.h"


#pragma region getters

D3D11Context GetD3D11Device() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
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
        ComPtr<ID3D11Multithread> pMultithread;
        hr = device.As(&pMultithread);
        if (SUCCEEDED(hr) && pMultithread) {
            pMultithread->SetMultithreadProtected(TRUE);
        }
    }

    D3D11Context result = {device, deviceContext};
    return result;
}

//factory
ComPtr<IDXGIFactory1> getFactory1() {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    return factory;
}

//adapters
std::vector<ComPtr<IDXGIAdapter1>> getAdapters1(IDXGIFactory1* factory) {
    int index = 0;
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        adapters.push_back(adapter);
        index++;
    }
    return adapters;
}

//outputs
std::vector<ComPtr<IDXGIOutput1>> getOutputs1(IDXGIAdapter1* adapter) {
    int index = 0;
    std::vector<ComPtr<IDXGIOutput1>> outputs;
    ComPtr<IDXGIOutput> output;
    while (adapter->EnumOutputs(index, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput1> output1;
        if (SUCCEEDED(output.As(&output1))) {
            outputs.push_back(output1);
        }
        index++;
    }
    return outputs;
}

//outputduplication
ComPtr<IDXGIOutputDuplication> getOutputDuplication(ID3D11Device* device, IDXGIOutput1* output1) {
    ComPtr<IDXGIOutputDuplication> outputDuplication;
    output1->DuplicateOutput(device, &outputDuplication);
    return outputDuplication;
}


#pragma region capture 

//resource (image)
ComPtr<IDXGIResource> getResource(IDXGIOutputDuplication* outputDuplication, UINT timeout) {
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> resource;
    outputDuplication->AcquireNextFrame(timeout, &frameInfo, &resource);
    return resource;
}


#pragma region decodage

//converti la ressource en texture
ComPtr<ID3D11Texture2D> resourceToTexture(IDXGIResource* resource) {
    ComPtr<ID3D11Texture2D> texture;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
    return texture;
}

//crée une texture relai lisable pour cpu (staging texture)
ComPtr<ID3D11Texture2D> createStagingTexture(ID3D11Device* device, ID3D11Texture2D* texture) {
    ComPtr<ID3D11Texture2D> stagingTexture;

    D3D11_TEXTURE2D_DESC desc; 
    texture->GetDesc(&desc);

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
    D3D11_MAPPED_SUBRESOURCE mapped;
    
    deviceContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    deviceContext->Unmap(stagingTexture, 0);
}
