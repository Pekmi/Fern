#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <strmif.h>
#include <codecapi.h>
#include <mferror.h>
#include <algorithm>
#include <cwctype>
#include <string>

#include "../include/fern/encoder.h"

namespace {
std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

bool IsHevcCodec(const VideoEncoderSettings& settings) {
    const std::wstring codec = ToLower(settings.codec);
    return codec == L"hevc" || codec == L"h265";
}

GUID VideoSubtype(const VideoEncoderSettings& settings) {
    return IsHevcCodec(settings) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

UINT32 VideoProfile(const VideoEncoderSettings& settings) {
    if (IsHevcCodec(settings)) return eAVEncH265VProfile_Main_420_8;
    return ToLower(settings.profile) == L"main"
        ? eAVEncH264VProfile_Main
        : eAVEncH264VProfile_High;
}

UINT32 RateControlMode(const VideoEncoderSettings& settings) {
    const std::wstring rateControl = ToLower(settings.rateControl);
    if (rateControl == L"cbr") return eAVEncCommonRateControlMode_CBR;
    if (rateControl == L"lowdelayvbr") return eAVEncCommonRateControlMode_LowDelayVBR;
    return eAVEncCommonRateControlMode_PeakConstrainedVBR;
}

void SetCodecApiUInt32(ICodecAPI* codecApi, const GUID& property, UINT32 value, const char* name) {
    if (!codecApi) return;

    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;

    const HRESULT hr = codecApi->SetValue(&property, &variant);
    if (FAILED(hr)) {
        std::cerr << "VIDEO ENCODER: option ignored " << name
                  << " 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void SetCodecApiBool(ICodecAPI* codecApi, const GUID& property, bool value, const char* name) {
    if (!codecApi) return;

    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;

    const HRESULT hr = codecApi->SetValue(&property, &variant);
    if (FAILED(hr)) {
        std::cerr << "VIDEO ENCODER: option ignored " << name
                  << " 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void ApplyVideoEncoderOptions(IMFTransform* encoder, const VideoEncoderSettings& settings) {
    ComPtr<ICodecAPI> codecApi;
    if (!encoder || FAILED(encoder->QueryInterface(IID_PPV_ARGS(&codecApi))) || !codecApi) return;

    const UINT32 fps = static_cast<UINT32>(std::max(1, settings.fps));
    const UINT32 bitrate = static_cast<UINT32>(std::max(1, settings.bitrateMbps)) * 1024u * 1024u;
    const UINT32 maxBitrate = bitrate * static_cast<UINT32>(std::clamp(settings.maxBitrateMultiplier, 100, 400)) / 100u;
    const UINT32 gopFrames = fps * static_cast<UINT32>(std::clamp(settings.gopSeconds, 1, 10));

    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, RateControlMode(settings), "RateControl");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate, maxBitrate, "MaxBitrate");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, static_cast<UINT32>(std::clamp(settings.qualityVsSpeed, 0, 100)), "QualityVsSpeed");
    SetCodecApiBool(codecApi.Get(), CODECAPI_AVEncCommonLowLatency, settings.lowLatency, "CommonLowLatency");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVLowLatencyMode, settings.lowLatency ? 1u : 0u, "LowLatencyMode");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncVideoMaxKeyframeDistance, gopFrames, "MaxKeyframeDistance");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, gopFrames, "GOPSize");
    SetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, static_cast<UINT32>(std::clamp(settings.bFrames, 0, 4)), "BFrames");

    if (!IsHevcCodec(settings)) {
        SetCodecApiBool(codecApi.Get(), CODECAPI_AVEncH264CABACEnable, true, "H264CABAC");
    }
}
}

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

HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, ComPtr<IMFTransform>& pEncoder, UINT width, UINT height, const VideoEncoderSettings& settings) {
    HRESULT hr = S_OK;
    UINT32 count = 0;
    IMFActivate** ppActivate = nullptr;
    VideoEncoderSettings effectiveSettings = settings;
    GUID subtype = VideoSubtype(effectiveSettings);
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, subtype };

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outputInfo, &ppActivate, &count);
    if ((FAILED(hr) || count == 0) && IsHevcCodec(settings)) {
        std::cerr << "VIDEO ENCODER: HEVC hardware encoder unavailable, falling back to H.264." << std::endl;
        effectiveSettings.codec = L"H264";
        subtype = MFVideoFormat_H264;
        outputInfo.guidSubtype = subtype;
        ppActivate = nullptr;
        count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outputInfo, &ppActivate, &count);
    }
    if (FAILED(hr) || count == 0) return E_FAIL;

    const UINT32 encoderIndex = static_cast<UINT32>(std::clamp(settings.encoderIndex, 0, static_cast<int>(count - 1)));
    hr = ppActivate[encoderIndex]->ActivateObject(IID_PPV_ARGS(&pEncoder));
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

    ApplyVideoEncoderOptions(pEncoder.Get(), effectiveSettings);

    ComPtr<IMFMediaType> pOutputType;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, subtype);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, std::max(1, effectiveSettings.bitrateMbps) * 1024 * 1024);
    MFSetAttributeSize(pOutputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pOutputType.Get(), MF_MT_FRAME_RATE, std::max(1, effectiveSettings.fps), 1);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, VideoProfile(effectiveSettings));

    hr = pEncoder->SetOutputType(0, pOutputType.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "VIDEO ENCODER ERROR: SetOutputType failed 0x" << std::hex << hr << std::dec << std::endl;
        return hr;
    }
    pEncoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)pDeviceManager);

    ComPtr<IMFMediaType> pInputType;
    if (SUCCEEDED(pEncoder->GetInputAvailableType(0, 0, &pInputType))) {
        MFSetAttributeSize(pInputType.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(pInputType.Get(), MF_MT_FRAME_RATE, std::max(1, effectiveSettings.fps), 1);
        hr = pEncoder->SetInputType(0, pInputType.Get(), 0);
        if (FAILED(hr)) {
            std::cerr << "VIDEO ENCODER ERROR: SetInputType failed 0x" << std::hex << hr << std::dec << std::endl;
            return hr;
        }
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
