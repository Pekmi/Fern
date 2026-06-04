#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#pragma region encoder

//constantes pour l'encodage
const LONGLONG HNS_PER_SEC = 10000000LL;

HRESULT InitializeMediaFoundation();
void ShutdownMediaFoundation();
struct DXGIDeviceManagerAndUInt {ComPtr<IMFDXGIDeviceManager> deviceManager; UINT resetToken;};
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device);

//init encodeur hardware MFT (Vidéo)
HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, ComPtr<IMFTransform>& pEncoder, UINT width, UINT height, int fps, int bitrateMbps);
//envoie une frame au MFT
HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp, LONGLONG durationHns);

//init encodeur audio AAC
HRESULT InitializeAudioEncoder(ComPtr<IMFTransform>& pEncoder, WAVEFORMATEX* pInputFormat);
//envoie du PCM au MFT audio
HRESULT PushAudioToEncoder(IMFTransform* pEncoder, IMFSample* pSample);

//récup sample compressé du MFT (Vidéo ou Audio)
HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, ComPtr<IMFSample>& pSample);