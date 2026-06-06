#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

#pragma region encoder

//constantes pour l'encodage
const LONGLONG HNS_PER_SEC = 10000000LL;

HRESULT InitializeMediaFoundation();
void ShutdownMediaFoundation();
struct DXGIDeviceManagerAndUInt {ComPtr<IMFDXGIDeviceManager> deviceManager; UINT resetToken;};
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device);

struct VideoEncoderSettings {
    int fps = 60;
    int bitrateMbps = 15;
    std::wstring codec = L"H264";
    std::wstring profile = L"High";
    std::wstring rateControl = L"VBR";
    int maxBitrateMultiplier = 200;
    int gopSeconds = 2;
    int bFrames = 2;
    bool lowLatency = false;
    int qualityVsSpeed = 70;
    int encoderIndex = 0;
};

//init encodeur hardware MFT (Vidéo)
HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, ComPtr<IMFTransform>& pEncoder, UINT width, UINT height, const VideoEncoderSettings& settings);
//envoie une frame au MFT
HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp, LONGLONG durationHns);

//init encodeur audio AAC
HRESULT InitializeAudioEncoder(ComPtr<IMFTransform>& pEncoder, WAVEFORMATEX* pInputFormat);
//envoie du PCM au MFT audio
HRESULT PushAudioToEncoder(IMFTransform* pEncoder, IMFSample* pSample);

//récup sample compressé du MFT (Vidéo ou Audio)
HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, ComPtr<IMFSample>& pSample);
