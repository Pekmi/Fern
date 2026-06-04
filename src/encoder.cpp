#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <codecapi.h>
#include <mferror.h>

#include "../include/fern/encoder.h"

HRESULT InitializeMediaFoundation() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return hr;
    return MFStartup(MF_VERSION);
}

void ShutdownMediaFoundation() {
    MFShutdown();
    CoUninitialize();
}

DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device) {
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
    if (FAILED(hr)) return {nullptr, 0};
    deviceManager->ResetDevice(device, resetToken);
    return {deviceManager, resetToken};
}

HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, ComPtr<IMFTransform>& pEncoder, UINT width, UINT height, int fps, int bitrateMbps) {
    HRESULT hr = S_OK;
    UINT32 count = 0;
    IMFActivate** ppActivate = nullptr;
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outputInfo, &ppActivate, &count);
    if (FAILED(hr) || count == 0) return E_FAIL;

    hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pEncoder));
    for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> pAttributes;
    if (SUCCEEDED(pEncoder->GetAttributes(&pAttributes))) {
        pAttributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        UINT32 isAsync = 0;
        if (SUCCEEDED(pAttributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            pAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
    }

    ComPtr<IMFMediaType> pOutputType;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateMbps * 1024 * 1024);
    MFSetAttributeSize(pOutputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pOutputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    pEncoder->SetOutputType(0, pOutputType.Get(), 0);
    pEncoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)pDeviceManager);

    ComPtr<IMFMediaType> pInputType;
    if (SUCCEEDED(pEncoder->GetInputAvailableType(0, 0, &pInputType))) {
        MFSetAttributeSize(pInputType.Get(), MF_MT_FRAME_SIZE, width, height);
        pEncoder->SetInputType(0, pInputType.Get(), 0);
    }
    return S_OK;
}

HRESULT InitializeAudioEncoder(ComPtr<IMFTransform>& pEncoder, WAVEFORMATEX* pInputFormat) {
    HRESULT hr = S_OK;
    UINT32 count = 0;
    IMFActivate** ppActivate = nullptr;
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };

    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &outputInfo, &ppActivate, &count);
    if (FAILED(hr) || count == 0) return E_FAIL;

    hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pEncoder));
    for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> pOutputType;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    pOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pInputFormat->nSamplesPerSec);
    pOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, pInputFormat->nChannels);
    pOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000); 
    pOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    
    hr = pEncoder->SetOutputType(0, pOutputType.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "AUDIO ENCODER ERROR: SetOutputType failed (0x" << std::hex << hr << ")" << std::endl;
        return hr;
    }

    ComPtr<IMFMediaType> pInputType;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); // L'encodeur AAC N'ACCEPTE QUE du PCM 16-bit entier
    
    pInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pInputFormat->nSamplesPerSec);
    pInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, pInputFormat->nChannels);
    pInputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); // On force 16 bits
    pInputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (16 / 8) * pInputFormat->nChannels);
    pInputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pInputFormat->nSamplesPerSec * (16 / 8) * pInputFormat->nChannels);

    hr = pEncoder->SetInputType(0, pInputType.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "AUDIO ENCODER ERROR: SetInputType failed (0x" << std::hex << hr << ")" << std::endl;
        return hr;
    }

    return S_OK;
}

HRESULT PushAudioToEncoder(IMFTransform* pEncoder, IMFSample* pSample) {
    if (!pEncoder || !pSample) return E_POINTER;
    return pEncoder->ProcessInput(0, pSample, 0);
}

HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp, LONGLONG durationHns) {
    ComPtr<IMFSample> pSample;
    MFCreateSample(&pSample);
    ComPtr<IMFMediaBuffer> pBuffer;
    MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pTexture, 0, FALSE, &pBuffer);
    pBuffer->SetCurrentLength(0);
    pSample->AddBuffer(pBuffer.Get());
    pSample->SetSampleTime(hnsTimestamp);
    pSample->SetSampleDuration(durationHns); 
    return pEncoder->ProcessInput(0, pSample.Get(), 0);
}

HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, ComPtr<IMFSample>& pSample) {
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    outputBuffer.dwStreamID = 0;
    outputBuffer.pSample = nullptr;
    outputBuffer.dwStatus = 0;
    outputBuffer.pEvents = nullptr;

    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = pEncoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return hr;

    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        // L'encodeur (comme l'AAC) ne fournit pas le sample, nous devons l'allouer
        hr = MFCreateSample(&outputBuffer.pSample);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaBuffer> pBuffer;
        hr = MFCreateMemoryBuffer(streamInfo.cbSize, &pBuffer);
        if (FAILED(hr)) { outputBuffer.pSample->Release(); return hr; }
        outputBuffer.pSample->AddBuffer(pBuffer.Get());
    }

    DWORD dwStatus = 0;
    hr = pEncoder->ProcessOutput(0, 1, &outputBuffer, &dwStatus);
    
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> pNewType;
        if (SUCCEEDED(pEncoder->GetOutputAvailableType(0, 0, &pNewType))) {
            HRESULT hrSet = pEncoder->SetOutputType(0, pNewType.Get(), 0);
            if (FAILED(hrSet)) {
                std::cerr << "SetOutputType ERROR: 0x" << std::hex << hrSet << std::dec << std::endl;
            }
            hr = pEncoder->ProcessOutput(0, 1, &outputBuffer, &dwStatus);
        }
    }

    if (hr == S_OK) {
        pSample.Attach(outputBuffer.pSample);
        if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
    } else {
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && hr != MF_E_TRANSFORM_STREAM_CHANGE) {
            std::cerr << "ProcessOutput ERROR: 0x" << std::hex << hr << std::dec << std::endl;
        }
        if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
        if (outputBuffer.pSample) outputBuffer.pSample->Release();
    }
    return hr;
}
