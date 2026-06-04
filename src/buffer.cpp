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

void RingBuffer::AddSample(IMFSample* pSample) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pSample) return;
    m_samples.push_back(pSample);
    EvictOldSamples();
}

void RingBuffer::EvictOldSamples() {
    if (m_samples.size() < 2) return;

    LONGLONG startTime = 0;
    LONGLONG endTime = 0;
    m_samples.front()->GetSampleTime(&startTime);
    m_samples.back()->GetSampleTime(&endTime);

    if (endTime - startTime > m_maxDuration) {
        while (m_samples.size() > 1) {
            LONGLONG nextStartTime = 0;
            m_samples[1]->GetSampleTime(&nextStartTime);

            //on ne peut s'arrêter que sur un CleanPoint (I-Frame)
            if (endTime - nextStartTime > m_maxDuration) {
                m_samples.pop_front();
                
                UINT32 isCleanPoint = 0;
                if (SUCCEEDED(m_samples.front()->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint)) && isCleanPoint) {
                    m_samples.front()->GetSampleTime(&startTime);
                    if (endTime - startTime <= m_maxDuration) break;
                }
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

std::deque<ComPtr<IMFSample>> RingBuffer::GetSnapshot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_samples; // Copie des pointeurs ComPtr
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

    LONGLONG hnsOffset = 0;
    m_samples.front()->GetSampleTime(&hnsOffset);

    for (size_t i = 0; i < m_samples.size(); ++i) {
        auto& pSample = m_samples[i];
        LONGLONG hnsOriginalTime = 0;
        pSample->GetSampleTime(&hnsOriginalTime);

        ComPtr<IMFSample> pOutputSample;
        hr = MFCreateSample(&pOutputSample);
        if (FAILED(hr)) break;

        UINT32 isCleanPoint = 0;
        if (SUCCEEDED(pSample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint))) {
            pOutputSample->SetUINT32(MFSampleExtension_CleanPoint, isCleanPoint);
        }

        if (i == 0) {
            pOutputSample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        }

        pOutputSample->SetSampleTime(hnsOriginalTime - hnsOffset);

        LONGLONG hnsDuration = 0;
        if (SUCCEEDED(pSample->GetSampleDuration(&hnsDuration))) {
            pOutputSample->SetSampleDuration(hnsDuration);
        }

        DWORD bufferCount = 0;
        pSample->GetBufferCount(&bufferCount);
        for (DWORD b = 0; b < bufferCount; b++) {
            ComPtr<IMFMediaBuffer> pBuffer;
            pSample->GetBufferByIndex(b, &pBuffer);
            pOutputSample->AddBuffer(pBuffer.Get());
        }

        hr = pSinkWriter->WriteSample(streamIndex, pOutputSample.Get());
        if (FAILED(hr)) break;
    }

    hr = pSinkWriter->Finalize();
    if (SUCCEEDED(hr)) std::cout << "Exportation terminee avec succes." << std::endl;
    
    return hr;
}