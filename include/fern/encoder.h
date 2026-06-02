#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#pragma region encoder

// Waitable timer and encoding constants
const LONGLONG HNS_PER_SEC = 10000000LL;
const int TARGET_FPS = 60;
const LONGLONG TICK_INTERVAL_HNS = HNS_PER_SEC / TARGET_FPS; // 166666 HNS for 60 FPS

HRESULT InitializeMediaFoundation();
void ShutdownMediaFoundation();
struct DXGIDeviceManagerAndUInt {ComPtr<IMFDXGIDeviceManager> deviceManager; UINT resetToken;};
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device);

//init encodeur hardware MFT
HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, ComPtr<IMFTransform>& pEncoder, UINT width, UINT height);
//envoie une frame au MFT
HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp);
//récup sample compressé du MFT
HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, ComPtr<IMFSample>& pSample);