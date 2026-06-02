#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <codecapi.h>
#include <mferror.h>

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

//cherche et configure l'encodeur hardware
HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, IMFTransform** ppEncoder, UINT width, UINT height) {
    HRESULT hr = S_OK;
    UINT32 count = 0;
    IMFActivate** ppActivate = nullptr;
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    //trouve l'encodeur hardware h264
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outputInfo, &ppActivate, &count);
    if (FAILED(hr) || count == 0) {
        std::cerr << "pas d'encodeur hardware h264 trouve" << std::endl;
        return (count == 0) ? E_FAIL : hr;
    }

    //active le premier encodeur trouvé
    LPWSTR pszName = nullptr;
    hr = ppActivate[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, nullptr);
    if (SUCCEEDED(hr)) {
        std::wcout << L"Encodeur choisi : " << pszName << std::endl;
        CoTaskMemFree(pszName);
    }

    ComPtr<IMFTransform> pEncoder;
    hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pEncoder));
    
    //cleanup des activations
    for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);

    if (FAILED(hr)) return hr;

    //annonce le support D3D11 et deverrouille l'asynchrone si besoin
    ComPtr<IMFAttributes> pAttributes;
    hr = pEncoder->GetAttributes(&pAttributes);
    if (SUCCEEDED(hr)) {
        pAttributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        UINT32 isAsync = 0;
        if (SUCCEEDED(pAttributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            std::cout << "Encodeur asynchrone detecte, deverrouillage..." << std::endl;
            pAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
    }

    //setup sortie
    ComPtr<IMFMediaType> pOutputType;
    hr = MFCreateMediaType(&pOutputType);
    if (FAILED(hr)) return hr;

    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, 10 * 1024 * 1024);
    MFSetAttributeSize(pOutputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pOutputType.Get(), MF_MT_FRAME_RATE, 60, 1);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    hr = pEncoder->SetOutputType(0, pOutputType.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "echec SetOutputType (hr=" << std::hex << hr << ")" << std::dec << std::endl;
        return hr;
    }

    //donne device manager
    hr = pEncoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)pDeviceManager);
    if (FAILED(hr)) {
        std::cerr << "echec liaison D3D manager (hr=" << std::hex << hr << ")" << std::dec << std::endl;
    }

    //setup entree
    ComPtr<IMFMediaType> pInputType;
    hr = pEncoder->GetInputAvailableType(0, 0, &pInputType);
    if (SUCCEEDED(hr)) {
        MFSetAttributeSize(pInputType.Get(), MF_MT_FRAME_SIZE, width, height);
        hr = pEncoder->SetInputType(0, pInputType.Get(), 0);
    }
    
    if (FAILED(hr)) {
        std::cerr << "echec SetInputType (hr=" << std::hex << hr << ")" << std::dec << std::endl;
        return hr;
    }

    *ppEncoder = pEncoder.Detach();
    return S_OK;
}

//envoie texture au MFT
HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp) {
    ComPtr<IMFSample> pSample;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaBuffer> pBuffer;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pTexture, 0, FALSE, &pBuffer);
    if (FAILED(hr)) return hr;

    //def longueur buffer
    pBuffer->SetCurrentLength(0);

    hr = pSample->AddBuffer(pBuffer.Get());
    if (FAILED(hr)) return hr;

    pSample->SetSampleTime(hnsTimestamp);
    pSample->SetSampleDuration(166666); 

    hr = pEncoder->ProcessInput(0, pSample.Get(), 0);
    return hr;
}

//recup données compressées
HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, IMFSample** ppSample) {
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    outputBuffer.dwStreamID = 0;
    outputBuffer.pSample = nullptr;
    outputBuffer.dwStatus = 0;
    outputBuffer.pEvents = nullptr;

    DWORD dwStatus = 0;
    HRESULT hr = pEncoder->ProcessOutput(0, 1, &outputBuffer, &dwStatus);
    
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        //encodeur veut changer de format (SPS/PPS?)
        ComPtr<IMFMediaType> pNewType;
        hr = pEncoder->GetOutputAvailableType(0, 0, &pNewType);
        if (SUCCEEDED(hr)) {
            hr = pEncoder->SetOutputType(0, pNewType.Get(), 0);
            if (SUCCEEDED(hr)) {
                //reessaie.
                hr = pEncoder->ProcessOutput(0, 1, &outputBuffer, &dwStatus);
            }
        }
    }

    if (hr == S_OK) {
        *ppSample = outputBuffer.pSample;
        if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
    } else {
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            std::cerr << "ProcessOutput erreur : " << std::hex << hr << std::dec << std::endl;
        }
    }
    
    return hr;
}