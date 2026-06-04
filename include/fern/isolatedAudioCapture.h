#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

class IsolatedAudioCapture {
public:

    IsolatedAudioCapture(DWORD targetPid);
    ~IsolatedAudioCapture();

    //init
    HRESULT Start();
    void Stop();

    //getters
    WAVEFORMATEX* GetFormat() { return (WAVEFORMATEX*)&m_mixFormat; }
    void SetStartTime(UINT64 qpc) { m_firstQpcTime = qpc; }

    //recup IMFSample
    HRESULT GetAudioSample(ComPtr<IMFSample>& pSample);

private:

    //gere callback asynchrone de wd
    class CompletionHandler;

    //gestion interne
    HRESULT InitializeAudioClient();
    void CaptureLoop();

    //propriétés de capture
    DWORD m_targetPid;
    WAVEFORMATEXTENSIBLE m_mixFormat;

    //interface wasapi
    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;

    //threading/synchro
    std::thread m_captureThread;
    std::atomic<bool> m_isCapturing;
    HANDLE m_sampleReadyEvent;
    HANDLE m_activationFinishedEvent;
    
    std::vector<short> m_pcmBuffer; 
    std::mutex m_audioMutex;

    UINT64 m_firstQpcTime;
    UINT64 m_framesSent;

};