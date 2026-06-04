#include <audioclientactivationparams.h>
#include <wrl/implements.h>
#include <iostream>
#include <mmdeviceapi.h>

#include "../include/fern/isolatedAudioCapture.h"

using namespace Microsoft::WRL;

class IsolatedAudioCapture::CompletionHandler : 
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceCompletionHandler> 
{
public:
    CompletionHandler(IsolatedAudioCapture* parent) : m_parent(parent) {}
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hr = S_OK, hrActivate = S_OK;
        ComPtr<IUnknown> punk;
        operation->GetActivateResult(&hrActivate, &punk);
        if (SUCCEEDED(hrActivate)) punk.As(&m_parent->m_audioClient);
        if (m_parent->m_activationFinishedEvent) SetEvent(m_parent->m_activationFinishedEvent);
        return S_OK;
    }
private:
    IsolatedAudioCapture* m_parent;
};

IsolatedAudioCapture::IsolatedAudioCapture(DWORD targetPid) : m_targetPid(targetPid), m_isCapturing(false), m_sampleReadyEvent(NULL), m_activationFinishedEvent(NULL), m_firstQpcTime(0), m_framesSent(0) {
    m_mixFormat = {};
}

IsolatedAudioCapture::~IsolatedAudioCapture() {
    Stop();
    if (m_sampleReadyEvent) CloseHandle(m_sampleReadyEvent);
    if (m_activationFinishedEvent) CloseHandle(m_activationFinishedEvent);
}

HRESULT IsolatedAudioCapture::Start() {
    if (m_isCapturing) return S_FALSE;
    m_activationFinishedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HRESULT hr = S_OK;
    if (m_targetPid != 0) {
        AUDIOCLIENT_ACTIVATION_PARAMS params = { AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK };
        params.ProcessLoopbackParams.TargetProcessId = m_targetPid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT prop; PropVariantInit(&prop); prop.vt = VT_BLOB; prop.blob.cbSize = sizeof(params); prop.blob.pBlobData = (BYTE*)&params;
        auto handler = Make<CompletionHandler>(this);
        ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &prop, handler.Get(), &asyncOp);
        if (FAILED(hr)) return hr;
        WaitForSingleObject(m_activationFinishedEvent, 2000);
    } else {
        ComPtr<IMMDeviceEnumerator> pEnum;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        ComPtr<IMMDevice> pDev;
        if (pEnum) {
            pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
            if (pDev) pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_audioClient);
        }
    }

    if (!m_audioClient) return E_FAIL;
    
    WAVEFORMATEX* pwfx = nullptr;
    m_audioClient->GetMixFormat(&pwfx);
    if (pwfx) {
        std::cout << "AUDIO: Format Detecte -> " << pwfx->nSamplesPerSec << "Hz, " << pwfx->nChannels << " canaux." << std::endl;
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) memcpy(&m_mixFormat, pwfx, sizeof(WAVEFORMATEXTENSIBLE));
        else { memset(&m_mixFormat, 0, sizeof(WAVEFORMATEXTENSIBLE)); memcpy(&m_mixFormat.Format, pwfx, sizeof(WAVEFORMATEX)); }
        CoTaskMemFree(pwfx);
    }

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, (WAVEFORMATEX*)&m_mixFormat, NULL);
    if (FAILED(hr)) { std::cerr << "AUDIO: Echec Initialize (0x" << std::hex << hr << ")" << std::endl; return hr; }
    
    m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    hr = m_audioClient->Start();
    if (FAILED(hr)) return hr;

    m_isCapturing = true;
    m_captureThread = std::thread(&IsolatedAudioCapture::CaptureLoop, this);
    return S_OK;
}

void IsolatedAudioCapture::Stop() {
    m_isCapturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_audioClient) m_audioClient->Stop();
}

HRESULT IsolatedAudioCapture::GetAudioSample(ComPtr<IMFSample>& pSample) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    
    UINT32 channels = m_mixFormat.Format.nChannels;
    UINT32 samplesNeeded = 1024 * channels;
    
    if (m_pcmBuffer.size() < samplesNeeded) return S_FALSE;
    
    DWORD bufferSize = samplesNeeded * sizeof(short);
    
    ComPtr<IMFMediaBuffer> pBuffer;
    MFCreateMemoryBuffer(bufferSize, &pBuffer);
    BYTE* pDest = nullptr;
    if (SUCCEEDED(pBuffer->Lock(&pDest, nullptr, nullptr))) {
        memcpy(pDest, m_pcmBuffer.data(), bufferSize);
        pBuffer->Unlock(); 
        pBuffer->SetCurrentLength(bufferSize);
    }
    
    MFCreateSample(&pSample);
    pSample->AddBuffer(pBuffer.Get());
    
    LONGLONG hnsTime = (LONGLONG)(m_framesSent * 10000000ULL / m_mixFormat.Format.nSamplesPerSec);
    pSample->SetSampleTime(hnsTime);
    
    LONGLONG hnsDuration = (LONGLONG)(1024 * 10000000ULL / m_mixFormat.Format.nSamplesPerSec);
    pSample->SetSampleDuration(hnsDuration);
    
    m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + samplesNeeded);
    m_framesSent += 1024;
    
    return S_OK;
}

void IsolatedAudioCapture::CaptureLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool firstData = true;
    while (m_isCapturing){
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UINT32 packetSize = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packetSize))) continue;
        while (packetSize > 0) {
            BYTE* pData = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 devPos = 0, qpc = 0;
            if (SUCCEEDED(m_captureClient->GetBuffer(&pData, &frames, &flags, &devPos, &qpc))) {
                if (firstData) { std::cout << "AUDIO: Premier paquet recu de WASAPI." << std::endl; firstData = false; }
                
                UINT32 numSamples = frames * m_mixFormat.Format.nChannels;
                std::vector<short> temp(numSamples, 0);

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    float* pFloat = (float*)pData;
                    for (UINT32 i = 0; i < numSamples; ++i) {
                        float sample = pFloat[i];
                        if (sample > 1.0f) sample = 1.0f;
                        if (sample < -1.0f) sample = -1.0f;
                        temp[i] = (short)(sample * 32767.0f);
                    }
                }
                
                { 
                    std::lock_guard<std::mutex> lock(m_audioMutex); 
                    m_pcmBuffer.insert(m_pcmBuffer.end(), temp.begin(), temp.end()); 
                }
                m_captureClient->ReleaseBuffer(frames);
            }
            m_captureClient->GetNextPacketSize(&packetSize);
        }
    }
    CoUninitialize();
}
