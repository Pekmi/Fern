#pragma once

#include "buffer.h"

#include <mfidl.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <vector>

namespace fern {

using Microsoft::WRL::ComPtr;

void AsyncSaveWorker(
    std::deque<StreamSample> samples,
    std::vector<ComPtr<IMFMediaType>> types,
    std::wstring filename,
    LONGLONG exportEndHns);

}
