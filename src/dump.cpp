#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <iostream>
#include "../include/fern/dump.h"



void AsyncDumpWorker(std::deque<Microsoft::WRL::ComPtr<IMFSample>> samples, Microsoft::WRL::ComPtr<IMFMediaType> pMediaType, std::wstring fileName) {
    if (samples.empty() || !pMediaType) {
        std::cerr << "[Dump] Erreur : samples vides ou media type invalide." << std::endl;
        return;
    }

    Microsoft::WRL::ComPtr<IMFSinkWriter> pSinkWriter;
    DWORD streamIndex = 0;
    
    HRESULT hr = MFCreateSinkWriterFromURL(fileName.c_str(), nullptr, nullptr, &pSinkWriter);
    if (FAILED(hr)) {
        std::cerr << "[Dump] Erreur creation SinkWriter : " << hr << std::endl;
        return;
    }

    hr = pSinkWriter->AddStream(pMediaType.Get(), &streamIndex);
    if (FAILED(hr)) {
        std::cerr << "[Dump] Erreur AddStream : " << hr << std::endl;
        return;
    }

    hr = pSinkWriter->BeginWriting();
    if (FAILED(hr)) {
        std::cerr << "[Dump] Erreur BeginWriting : " << hr << std::endl;
        return;
    }

    std::wcout << L"[Dump] Debut ecriture asynchrone : " << fileName << L" (" << samples.size() << L" samples)" << std::endl;

    LONGLONG hnsOffset = 0;
    samples.front()->GetSampleTime(&hnsOffset);

    for (size_t i = 0; i < samples.size(); ++i) {
        auto& pSample = samples[i];
        LONGLONG hnsOriginalTime = 0;
        pSample->GetSampleTime(&hnsOriginalTime);

        Microsoft::WRL::ComPtr<IMFSample> pOutputSample;
        hr = MFCreateSample(&pOutputSample);
        if (FAILED(hr)) break;

        //copie que cleanpoint
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
            Microsoft::WRL::ComPtr<IMFMediaBuffer> pBuffer;
            pSample->GetBufferByIndex(b, &pBuffer);
            pOutputSample->AddBuffer(pBuffer.Get());
        }

        hr = pSinkWriter->WriteSample(streamIndex, pOutputSample.Get());
        if (FAILED(hr)) break;
    }

    hr = pSinkWriter->Finalize();
    if (SUCCEEDED(hr)) {
        std::wcout << L"[Dump] Succes : " << fileName << std::endl;
    } else {
        std::wcerr << L"[Dump] Erreur lors de la finalisation : " << hr << std::endl;
    }
}
