#pragma once

#include "buffer.h"
#include "audio_activity.h"

#include <mfidl.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <vector>

namespace fern {

using Microsoft::WRL::ComPtr;

struct AudioTrackMetadata {
    DWORD streamIndex = 0;
    DWORD pid = 0;
    std::wstring name;
    std::vector<AudioActivityRange> activityRanges;
    LONGLONG activeDurationHns = 0;
    double activeRatio = 0.0;
};

void AsyncSaveWorker(
    std::deque<StreamSample> samples,
    std::vector<ComPtr<IMFMediaType>> types,
    std::vector<AudioTrackMetadata> audioTrackMetadata,
    std::wstring filename,
    LONGLONG exportEndHns);

}
