#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>

#include "../include/fern/encoder.h"


#pragma region setup

//init media foundation
HRESULT InitializeMediaFoundation() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        return hr;
    }
    return MFStartup(MF_VERSION);
}

//tue media foundation
void ShutdownMediaFoundation() {
    MFShutdown();
    CoUninitialize();
}


#pragma region 

//cree DXGI device manager
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device) {
    IMFDXGIDeviceManager* deviceManager = nullptr;
    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
    if (FAILED(hr)) {
        std::cerr << "erreur creation du DXGI device manager : " << hr << std::endl;
        return {nullptr, 0};
    }
    deviceManager->ResetDevice(device, resetToken);
    return {deviceManager, resetToken};
}

//cree le sink writer
HRESULT CreateSinkWriter(LPCWSTR pwszOutputURL, IMFDXGIDeviceManager* pDeviceManager, IMFSinkWriter** ppSinkWriter, DWORD* pwdStreamIndex, UINT width, UINT height) {
    IMFSinkWriter* pSinkWriter = nullptr;
    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) { return hr; }
    pAttributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, pDeviceManager);
    pAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    hr = MFCreateSinkWriterFromURL(pwszOutputURL, nullptr, pAttributes, &pSinkWriter);
    if (FAILED(hr)) { return hr; }
    *ppSinkWriter = pSinkWriter;
    *pwdStreamIndex = 0;
    pAttributes->Release();

    //encoder en H264
    IMFMediaType* pMediaTypeOut = nullptr;
    hr = MFCreateMediaType(&pMediaTypeOut);
    if (FAILED(hr)) { return hr; }
    pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, 8*1024*1024); //8mbps
    pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pMediaTypeOut, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pMediaTypeOut, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(pMediaTypeOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = pSinkWriter->AddStream(pMediaTypeOut, pwdStreamIndex);
    if (FAILED(hr)) { return hr; }
    pMediaTypeOut->Release();

    IMFMediaType* pMediaTypeIn = nullptr;
    hr = MFCreateMediaType(&pMediaTypeIn);
    if (FAILED(hr)) { return hr; }
    pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    pMediaTypeIn->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
    MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = pSinkWriter->SetInputMediaType(*pwdStreamIndex, pMediaTypeIn, nullptr);
    if (FAILED(hr)) { return hr; }
    pMediaTypeIn->Release();

    pSinkWriter->BeginWriting();

    return S_OK;
}

//encode une frame
HRESULT EncodeFrame(IMFSinkWriter* pSinkWriter, DWORD streamIndex, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp) {
    IMFSample* pSample = nullptr;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr)) { return hr; }
    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pTexture, 0, FALSE, &pBuffer);
    if (FAILED(hr)) { return hr; }

    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);
    pBuffer->SetCurrentLength(desc.Width * desc.Height * 4);

    hr = pSample->AddBuffer(pBuffer);
    if (FAILED(hr)) { return hr; }
    pBuffer->Release();
    pSample->SetSampleTime(hnsTimestamp);
    // pSample->SetSampleDuration(166666); //16.666ms c'est 60fps
    hr = pSinkWriter->WriteSample(streamIndex, pSample);
    if (FAILED(hr)) { return hr; }
    pSample->Release();
    
    return S_OK;
}