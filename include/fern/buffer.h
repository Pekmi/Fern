#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <deque>
#include <mutex>

using Microsoft::WRL::ComPtr;

// gère le tampon circulaire de samples compressés en RAM
class RingBuffer {
public:
    RingBuffer(LONGLONG maxDurationHNS);
    ~RingBuffer();

    // ajoute un sample et gère l'éviction
    void AddSample(IMFSample* pSample);
    
    // retourne le nombre de samples actuellement en mémoire
    size_t GetSampleCount();

    // vide le buffer
    void Clear();

    // sauvegarde le contenu actuel dans un fichier mp4
    HRESULT SaveToFile(LPCWSTR pwszFileName, IMFMediaType* pMediaType);

private:
    std::deque<ComPtr<IMFSample>> m_samples;
    LONGLONG m_maxDuration; // durée max en unités de 100ns
    std::mutex m_mutex;

    void EvictOldSamples();
};