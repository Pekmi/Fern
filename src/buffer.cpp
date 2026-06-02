#include "../include/fern/buffer.h"
#include <mfreadwrite.h>
#include <mferror.h>
#include <iostream>

RingBuffer::RingBuffer(LONGLONG maxDurationHNS) : m_maxDuration(maxDurationHNS) {
}

RingBuffer::~RingBuffer() {
    Clear();
}

void RingBuffer::AddSample(IMFSample* pSample) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pSample) return;
    m_samples.push_back(pSample);
    EvictOldSamples();
}

void RingBuffer::EvictOldSamples() {
    if (m_samples.empty()) return;
    LONGLONG startTime = 0;
    LONGLONG endTime = 0;
    m_samples.front()->GetSampleTime(&startTime);
    m_samples.back()->GetSampleTime(&endTime);

    if (endTime - startTime > m_maxDuration) {
        while (m_samples.size() > 1) {
            m_samples.front()->GetSampleTime(&startTime);
            if (endTime - startTime <= m_maxDuration) break;
            m_samples.pop_front();
            UINT32 isCleanPoint = 0;
            if (FAILED(m_samples.front()->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint)) || !isCleanPoint) {
                continue; 
            } else {
                break;
            }
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

HRESULT RingBuffer::SaveToFile(LPCWSTR pwszFileName, IMFMediaType* pMediaType) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) return E_FAIL;

    ComPtr<IMFSinkWriter> pSinkWriter;
    DWORD streamIndex = 0;
    
    HRESULT hr = MFCreateSinkWriterFromURL(pwszFileName, nullptr, nullptr, &pSinkWriter);
    if (FAILED(hr)) return hr;

    hr = pSinkWriter->AddStream(pMediaType, &streamIndex);
    if (FAILED(hr)) return hr;

    hr = pSinkWriter->BeginWriting();
    if (FAILED(hr)) return hr;

    std::cout << "Exportation de " << m_samples.size() << " samples vers clip.mp4..." << std::endl;

    for (auto& pSample : m_samples) {
        hr = pSinkWriter->WriteSample(streamIndex, pSample.Get());
        if (FAILED(hr)) break;
    }

    hr = pSinkWriter->Finalize();
    if (SUCCEEDED(hr)) std::cout << "Exportation terminee avec succes." << std::endl;
    
    return hr;
}