#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <deque>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

//associe un sample à son index de flux (0=Vidéo, 1=Audio1, etc.)
struct StreamSample {
    ComPtr<IMFSample> sample;
    DWORD streamIndex;
};

//gère le tampon circulaire de samples compressés en RAM
class RingBuffer {
public:
    RingBuffer(LONGLONG maxDurationHNS);
    ~RingBuffer();

    //ajoute un sample avec index de flux
    void AddSample(IMFSample* pSample, DWORD streamIndex);
    
    //retourne le nombre de samples actuellement en mémoire
    size_t GetSampleCount();

    //vide le buffer
    void Clear();

    std::deque<StreamSample> GetSnapshot();

    //sauvegarde contenu dans mp4 multi-pistes
    HRESULT SaveToFile(LPCWSTR pwszFileName, const std::vector<ComPtr<IMFMediaType>>& pMediaTypes);

private:
    std::deque<StreamSample> m_samples;
    LONGLONG m_maxDuration; //durée max x100ns
    LONGLONG m_latestTime = 0;
    bool m_hasLatestTime = false;
    std::mutex m_mutex;

    void EvictOldSamples();
};
