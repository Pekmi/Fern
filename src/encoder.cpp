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
#include <cstring>
#include <cwctype>
#include <sstream>
#include <string>

#include "../include/fern/encoder.h"
#include "../include/fern/logger.h"

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

std::wstring ActivateName(IMFActivate* activate) {
    if (!activate) return L"<null>";

    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &value, &length)) && value) {
        std::wstring result(value, length);
        CoTaskMemFree(value);
        return result;
    }

    return L"<unnamed>";
}

std::wstring VideoSettingsText(const VideoEncoderSettings& settings, UINT width, UINT height) {
    std::wostringstream stream;
    stream << L"codec=" << settings.codec
           << L" size=" << width << L"x" << height
           << L" fps=" << settings.fps
           << L" bitrateMbps=" << settings.bitrateMbps
           << L" profile=" << settings.profile
           << L" rateControl=" << settings.rateControl
           << L" maxBitrateMultiplier=" << settings.maxBitrateMultiplier
           << L" gopSeconds=" << settings.gopSeconds
           << L" bFrames=" << settings.bFrames
           << L" lowLatency=" << (settings.lowLatency ? L"true" : L"false")
           << L" qualityVsSpeed=" << settings.qualityVsSpeed
           << L" encoderIndex=" << settings.encoderIndex;
    return stream.str();
}

void ReleaseActivates(IMFActivate** activates, UINT32 count) {
    if (!activates) return;
    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);
}

UINT EvenDimension(UINT value) {
    return std::max<UINT>(2, value & ~1u);
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
        fern::LogHResult(fern::LogLevel::Warning, L"VIDEO_ENCODER", L"Option ignored " + std::wstring(name, name + strlen(name)), hr);
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
        fern::LogHResult(fern::LogLevel::Warning, L"VIDEO_ENCODER", L"Option ignored " + std::wstring(name, name + strlen(name)), hr);
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

HRESULT InitializeHardwareEncoder(
    IMFDXGIDeviceManager* pDeviceManager,
    ComPtr<IMFTransform>& pEncoder,
    UINT width,
    UINT height,
    const VideoEncoderSettings& settings,
    VideoEncoderRuntime* runtime) {
    pEncoder.Reset();
    if (runtime) {
        runtime->inputMode = VideoEncoderInputMode::D3D11Texture;
        runtime->width = width;
        runtime->height = height;
    }

    auto configureOutput = [&](IMFTransform* encoder, const VideoEncoderSettings& effectiveSettings, GUID subtype, UINT outputWidth, UINT outputHeight) {
        ComPtr<IMFMediaType> outputType;
        HRESULT hr = MFCreateMediaType(&outputType);
        if (FAILED(hr)) return hr;

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, subtype);
        outputType->SetUINT32(MF_MT_AVG_BITRATE, std::max(1, effectiveSettings.bitrateMbps) * 1024 * 1024);
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, outputWidth, outputHeight);
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, std::max(1, effectiveSettings.fps), 1);
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outputType->SetUINT32(MF_MT_MPEG2_PROFILE, VideoProfile(effectiveSettings));
        return encoder->SetOutputType(0, outputType.Get(), 0);
    };

    auto configureHardware = [&](ComPtr<IMFTransform>& encoder, const VideoEncoderSettings& effectiveSettings, GUID subtype) {
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes) {
            attributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
            UINT32 isAsync = 0;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }

        ApplyVideoEncoderOptions(encoder.Get(), effectiveSettings);

        HRESULT hr = configureOutput(encoder.Get(), effectiveSettings, subtype, width, height);
        if (FAILED(hr)) return hr;

        encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(pDeviceManager));

        ComPtr<IMFMediaType> inputType;
        if (SUCCEEDED(encoder->GetInputAvailableType(0, 0, &inputType)) && inputType) {
            MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
            MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, std::max(1, effectiveSettings.fps), 1);
            hr = encoder->SetInputType(0, inputType.Get(), 0);
            if (FAILED(hr)) return hr;
        }

        return S_OK;
    };

    auto tryHardware = [&](VideoEncoderSettings effectiveSettings, HRESULT& enumHr) {
        UINT32 count = 0;
        IMFActivate** activates = nullptr;
        GUID subtype = VideoSubtype(effectiveSettings);
        MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, subtype };

        enumHr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outputInfo,
            &activates,
            &count);

        if (FAILED(enumHr) || count == 0) {
            ReleaseActivates(activates, count);
            return E_FAIL;
        }

        {
            std::wostringstream stream;
            stream << L"Hardware encoder candidates=" << count << L" " << VideoSettingsText(effectiveSettings, width, height);
            for (UINT32 i = 0; i < count; ++i) {
                stream << L" [" << i << L"]=" << ActivateName(activates[i]);
            }
            fern::LogInfo(L"VIDEO_ENCODER", stream.str());
        }

        const UINT32 encoderIndex = static_cast<UINT32>(std::clamp(settings.encoderIndex, 0, static_cast<int>(count - 1)));
        fern::LogInfo(L"VIDEO_ENCODER", L"Selected hardware encoder [" + std::to_wstring(encoderIndex) + L"]=" + ActivateName(activates[encoderIndex]));

        ComPtr<IMFTransform> candidate;
        HRESULT hr = activates[encoderIndex]->ActivateObject(IID_PPV_ARGS(&candidate));
        ReleaseActivates(activates, count);
        if (FAILED(hr)) return hr;

        hr = configureHardware(candidate, effectiveSettings, subtype);
        if (FAILED(hr)) return hr;

        pEncoder = candidate;
        if (runtime) {
            runtime->inputMode = VideoEncoderInputMode::D3D11Texture;
            runtime->width = width;
            runtime->height = height;
        }
        fern::LogInfo(L"VIDEO_ENCODER", L"Hardware video encoder initialized.");
        return S_OK;
    };

    VideoEncoderSettings effectiveSettings = settings;
    HRESULT enumHr = S_OK;
    HRESULT hr = tryHardware(effectiveSettings, enumHr);
    if (SUCCEEDED(hr)) return S_OK;

    if (IsHevcCodec(settings)) {
        std::cerr << "VIDEO ENCODER: HEVC hardware encoder unavailable, falling back to H.264." << std::endl;
        fern::LogWarning(L"VIDEO_ENCODER", L"HEVC hardware encoder unavailable; falling back to H.264.");
        effectiveSettings.codec = L"H264";
        hr = tryHardware(effectiveSettings, enumHr);
        if (SUCCEEDED(hr)) return S_OK;
    }

    fern::LogHResult(
        fern::LogLevel::Warning,
        L"VIDEO_ENCODER",
        L"No hardware encoder found; trying software H.264 for " + VideoSettingsText(effectiveSettings, width, height),
        FAILED(enumHr) ? enumHr : E_FAIL);

    VideoEncoderSettings softwareSettings = effectiveSettings;
    softwareSettings.codec = L"H264";
    const GUID softwareSubtype = MFVideoFormat_H264;
    const UINT softwareWidth = EvenDimension(width);
    const UINT softwareHeight = EvenDimension(height);
    if (softwareWidth != width || softwareHeight != height) {
        std::wostringstream stream;
        stream << L"Software encoder will crop frame from " << width << L"x" << height
               << L" to " << softwareWidth << L"x" << softwareHeight << L".";
        fern::LogWarning(L"VIDEO_ENCODER", stream.str());
    }

    UINT32 count = 0;
    IMFActivate** activates = nullptr;
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, softwareSubtype };
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outputInfo,
        &activates,
        &count);
    if (FAILED(hr) || count == 0) {
        ReleaseActivates(activates, count);
        fern::LogHResult(
            fern::LogLevel::Error,
            L"VIDEO_ENCODER",
            L"No software H.264 encoder found for " + VideoSettingsText(softwareSettings, softwareWidth, softwareHeight),
            FAILED(hr) ? hr : E_FAIL);
        return E_FAIL;
    }

    {
        std::wostringstream stream;
        stream << L"Software encoder candidates=" << count << L" " << VideoSettingsText(softwareSettings, softwareWidth, softwareHeight);
        for (UINT32 i = 0; i < count; ++i) {
            stream << L" [" << i << L"]=" << ActivateName(activates[i]);
        }
        fern::LogInfo(L"VIDEO_ENCODER", stream.str());
    }

    fern::LogInfo(L"VIDEO_ENCODER", L"Selected software encoder [0]=" + ActivateName(activates[0]));
    ComPtr<IMFTransform> softwareEncoder;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&softwareEncoder));
    ReleaseActivates(activates, count);
    if (FAILED(hr) || !softwareEncoder) {
        fern::LogHResult(fern::LogLevel::Error, L"VIDEO_ENCODER", L"Software ActivateObject failed.", FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }

    ApplyVideoEncoderOptions(softwareEncoder.Get(), softwareSettings);

    hr = configureOutput(softwareEncoder.Get(), softwareSettings, softwareSubtype, softwareWidth, softwareHeight);
    if (FAILED(hr)) {
        fern::LogHResult(fern::LogLevel::Error, L"VIDEO_ENCODER", L"Software SetOutputType failed.", hr);
        return hr;
    }

    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return hr;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, softwareWidth, softwareHeight);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, std::max(1, softwareSettings.fps), 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = softwareEncoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        fern::LogHResult(fern::LogLevel::Error, L"VIDEO_ENCODER", L"Software SetInputType NV12 failed.", hr);
        return hr;
    }

    pEncoder = softwareEncoder;
    if (runtime) {
        runtime->inputMode = VideoEncoderInputMode::SoftwareNv12;
        runtime->width = softwareWidth;
        runtime->height = softwareHeight;
    }
    fern::LogInfo(L"VIDEO_ENCODER", L"Software H.264 video encoder initialized.");
    return S_OK;
}

HRESULT InitializeAudioEncoder(ComPtr<IMFTransform>& pEncoder, WAVEFORMATEX* pInputFormat) {
    HRESULT hr = S_OK;
    UINT32 count = 0;
    IMFActivate** ppActivate = nullptr;
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };

    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &outputInfo, &ppActivate, &count);
    if (FAILED(hr) || count == 0) {
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO_ENCODER", L"No AAC encoder found.", FAILED(hr) ? hr : E_FAIL);
        return E_FAIL;
    }
    fern::LogInfo(L"AUDIO_ENCODER", L"AAC encoder candidates=" + std::to_wstring(count) + L" selected=" + ActivateName(ppActivate[0]));

    hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pEncoder));
    for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);
    if (FAILED(hr)) {
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO_ENCODER", L"ActivateObject failed.", hr);
        return hr;
    }

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
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO_ENCODER", L"SetOutputType failed.", hr);
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
        fern::LogHResult(fern::LogLevel::Warning, L"AUDIO_ENCODER", L"SetInputType failed.", hr);
        return hr;
    }

    fern::LogInfo(L"AUDIO_ENCODER", L"Audio encoder initialized.");
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

HRESULT CreateSoftwareVideoStagingTexture(ID3D11Device* device, UINT width, UINT height, ComPtr<ID3D11Texture2D>& texture) {
    texture.Reset();
    if (!device || width == 0 || height == 0) return E_POINTER;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    return device->CreateTexture2D(&desc, nullptr, &texture);
}

namespace {

BYTE ClampToByte(int value) {
    return static_cast<BYTE>(std::clamp(value, 0, 255));
}

void BgraPixelToYuv(const BYTE* pixel, int& y, int& u, int& v) {
    const int b = pixel[0];
    const int g = pixel[1];
    const int r = pixel[2];

    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

void ConvertBgraToNv12(const BYTE* bgra, LONG bgraStride, UINT width, UINT height, BYTE* nv12) {
    BYTE* yPlane = nv12;
    BYTE* uvPlane = nv12 + static_cast<size_t>(width) * height;

    for (UINT y = 0; y < height; ++y) {
        const BYTE* row = bgra + static_cast<size_t>(y) * bgraStride;
        BYTE* yRow = yPlane + static_cast<size_t>(y) * width;

        for (UINT x = 0; x < width; ++x) {
            int yy = 0;
            int uu = 0;
            int vv = 0;
            BgraPixelToYuv(row + static_cast<size_t>(x) * 4, yy, uu, vv);
            yRow[x] = ClampToByte(yy);
        }
    }

    for (UINT y = 0; y < height; y += 2) {
        const BYTE* row0 = bgra + static_cast<size_t>(y) * bgraStride;
        const BYTE* row1 = bgra + static_cast<size_t>(y + 1) * bgraStride;
        BYTE* uvRow = uvPlane + static_cast<size_t>(y / 2) * width;

        for (UINT x = 0; x < width; x += 2) {
            int uSum = 0;
            int vSum = 0;
            for (UINT dy = 0; dy < 2; ++dy) {
                const BYTE* row = dy == 0 ? row0 : row1;
                for (UINT dx = 0; dx < 2; ++dx) {
                    int yy = 0;
                    int uu = 0;
                    int vv = 0;
                    BgraPixelToYuv(row + static_cast<size_t>(x + dx) * 4, yy, uu, vv);
                    uSum += uu;
                    vSum += vv;
                }
            }

            uvRow[x] = ClampToByte((uSum + 2) / 4);
            uvRow[x + 1] = ClampToByte((vSum + 2) / 4);
        }
    }
}

}

HRESULT PushSoftwareFrameToEncoder(
    IMFTransform* pEncoder,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* sourceTexture,
    ID3D11Texture2D* stagingTexture,
    UINT width,
    UINT height,
    LONGLONG hnsTimestamp,
    LONGLONG durationHns) {
    if (!pEncoder || !context || !sourceTexture || !stagingTexture || width == 0 || height == 0) return E_POINTER;

    D3D11_BOX sourceBox = {};
    sourceBox.left = 0;
    sourceBox.top = 0;
    sourceBox.front = 0;
    sourceBox.right = width;
    sourceBox.bottom = height;
    sourceBox.back = 1;
    context->CopySubresourceRegion(stagingTexture, 0, 0, 0, 0, sourceTexture, 0, &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    const DWORD bufferSize = static_cast<DWORD>(static_cast<size_t>(width) * height * 3 / 2);
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(hr)) {
        context->Unmap(stagingTexture, 0);
        return hr;
    }

    BYTE* destination = nullptr;
    hr = buffer->Lock(&destination, nullptr, nullptr);
    if (FAILED(hr)) {
        context->Unmap(stagingTexture, 0);
        return hr;
    }

    ConvertBgraToNv12(
        static_cast<const BYTE*>(mapped.pData),
        static_cast<LONG>(mapped.RowPitch),
        width,
        height,
        destination);

    buffer->Unlock();
    context->Unmap(stagingTexture, 0);
    buffer->SetCurrentLength(bufferSize);

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(hnsTimestamp);
    sample->SetSampleDuration(durationHns);
    return pEncoder->ProcessInput(0, sample.Get(), 0);
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
