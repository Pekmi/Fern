#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#pragma region encoder

HRESULT InitializeMediaFoundation();
void ShutdownMediaFoundation();
struct DXGIDeviceManagerAndUInt {IMFDXGIDeviceManager* deviceManager; UINT resetToken;};
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device);

//init encodeur hardware MFT
HRESULT InitializeHardwareEncoder(IMFDXGIDeviceManager* pDeviceManager, IMFTransform** ppEncoder, UINT width, UINT height);
//envoie une frame au MFT
HRESULT PushFrameToEncoder(IMFTransform* pEncoder, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp);
//récup sample compressé du MFT
HRESULT PullSampleFromEncoder(IMFTransform* pEncoder, IMFSample** ppSample);