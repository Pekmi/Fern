#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <d3d11.h>


#pragma region encoder

HRESULT InitializeMediaFoundation();
void ShutdownMediaFoundation();
struct DXGIDeviceManagerAndUInt {IMFDXGIDeviceManager* deviceManager; UINT resetToken;};
DXGIDeviceManagerAndUInt CreateDXGIDeviceManager(ID3D11Device* device);
HRESULT CreateSinkWriter(LPCWSTR pwszOutputURL, IMFDXGIDeviceManager* pDeviceManager, IMFSinkWriter** ppSinkWriter, DWORD* pwdStreamIndex, UINT width, UINT height);
HRESULT EncodeFrame(IMFSinkWriter* pSinkWriter, DWORD streamIndex, ID3D11Texture2D* pTexture, LONGLONG hnsTimestamp);