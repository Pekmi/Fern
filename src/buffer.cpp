#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <iostream>

#include "../include/fern/buffer.h"


RingBuffer::RingBuffer(LONGLONG maxDurationHNS) : m_maxDuration(maxDurationHNS) {
}

RingBuffer::~RingBuffer() {
    Clear();
}

void RingBuffer::AddSample(IMFSample* pSample, DWORD streamIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pSample) return;
    
    StreamSample ss;
    ss.sample = pSample;
    ss.streamIndex = streamIndex;
    
    m_samples.push_back(std::move(ss));
    EvictOldSamples();
}

void RingBuffer::EvictOldSamples() {
    if (m_samples.size() < 2) return;

    LONGLONG latestTime = 0;
    m_samples.back().sample->GetSampleTime(&latestTime);

    while (m_samples.size() > 1) {
        LONGLONG firstTime = 0;
        m_samples.front().sample->GetSampleTime(&firstTime);

        //nettoyage basé sur la durée max
        if (latestTime - firstTime > m_maxDuration) {
            m_samples.pop_front();
        } else {
            break;
        }
    }
}

size_t RingBuffer::GetSampleCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_samples.size();
}

void RingBuffer::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
}

std::deque<StreamSample> RingBuffer::GetSnapshot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_samples;
}

HRESULT RingBuffer::SaveToFile(LPCWSTR pwszFileName, const std::vector<ComPtr<IMFMediaType>>& pMediaTypes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) return E_FAIL;

    ComPtr<IMFSinkWriter> pSinkWriter;
    HRESULT hr = MFCreateSinkWriterFromURL(pwszFileName, nullptr, nullptr, &pSinkWriter);
    if (FAILED(hr)) return hr;

    std::vector<DWORD> sinkStreamIndices;
    for (size_t i = 0; i < pMediaTypes.size(); ++i) {
        DWORD dwIndex = 0;
        hr = pSinkWriter->AddStream(pMediaTypes[i].Get(), &dwIndex);
        if (FAILED(hr)) return hr;
        sinkStreamIndices.push_back(dwIndex);
    }

    hr = pSinkWriter->BeginWriting();
    if (FAILED(hr)) return hr;

    LONGLONG hnsOffset = -1;

    for (auto& ss : m_samples) {
        LONGLONG hnsTime = 0;
        ss.sample->GetSampleTime(&hnsTime);
        if (hnsOffset == -1) hnsOffset = hnsTime;

        ComPtr<IMFSample> pOutSample;
        hr = MFCreateSample(&pOutSample);
        if (FAILED(hr)) break;
        
        ss.sample->CopyAllItems(pOutSample.Get());
        pOutSample->SetSampleTime(hnsTime - hnsOffset);
        
        LONGLONG duration = 0;
        if (SUCCEEDED(ss.sample->GetSampleDuration(&duration))) {
            pOutSample->SetSampleDuration(duration);
        }

        DWORD bufCount = 0;
        ss.sample->GetBufferCount(&bufCount);
        for (DWORD i = 0; i < bufCount; i++) {
            ComPtr<IMFMediaBuffer> pBuffer;
            ss.sample->GetBufferByIndex(i, &pBuffer);
            pOutSample->AddBuffer(pBuffer.Get());
        }

        pSinkWriter->WriteSample(sinkStreamIndices[ss.streamIndex], pOutSample.Get());
    }

    return pSinkWriter->Finalize();
}
