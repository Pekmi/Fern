#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <deque>
#include <string>
#include <wrl/client.h>


//dump asynchrone
void AsyncDumpWorker(std::deque<Microsoft::WRL::ComPtr<IMFSample>> samples, Microsoft::WRL::ComPtr<IMFMediaType> pMediaType, std::wstring fileName);
