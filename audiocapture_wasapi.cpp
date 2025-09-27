//
// Copyright (C) Tammo Hinrichs 2021. All rights reserved.
// Licensed under the MIT License. See LICENSE.md file for full license information
//

#include "system.h"
#include "audiocapture.h"
#include "screencapture.h"

#include <stdio.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>

extern const char* ErrorString(HRESULT id);
#if _DEBUG
#define CHECK(x) { HRESULT _hr=(x); if(FAILED(_hr)) Fatal("%s(%d): WASAPI call failed: %s\nCall: %s\n",__FILE__,__LINE__,ErrorString(_hr),#x); }
#else
#define CHECK(x) { HRESULT _hr=(x); if(FAILED(_hr)) Fatal("%s(%d): WASAPI call failed: %s\n",__FILE__,__LINE__,ErrorString(_hr)); }
#endif

static constexpr int REFPERSEC = 10000000;

// マイクとシステム音デバイスを区別できるように、構造体として保持する
struct AudioDevice
{
    RCPtr<IMMDevice> Device;
    EDataFlow Flow; // eRender (出力/システム音) または eCapture (入力/マイク)
    bool IsDefault = false; // デフォルトデバイスかどうかのフラグ
};
static Array<AudioDevice> Devices;

class AudioCapture_WASAPI : public IAudioCapture
{
    const CaptureConfig& Config;

    RCPtr<IAudioClient> Client;
    RCPtr<IAudioCaptureClient> CaptureClient;

    RCPtr<IAudioClient> PlaybackClient; // システム音キャプチャでのみ使用

    WAVEFORMATEXTENSIBLE* Format = nullptr;
    uint BufferSize = 0;

    Thread* CaptureThread = nullptr;

    uint8* Ring = nullptr;
    uint RingSize = 0;
    uint RingRead = 0;
    uint RingWrite = 0;
    uint RingTimePos = 0;
    double RingTimeValue = 0;
    ThreadLock RingLock;

    uint BytesPerSample = 0;
    AudioInfo Info = {};

    void CaptureThreadFunc(Thread& thread)
    {
        const int bufferMs = 1000 * BufferSize / Format->Format.nSamplesPerSec;

        while (thread.Wait(bufferMs / 2))
        {
            uint packetSize = 0;
            CHECK(CaptureClient->GetNextPacketSize(&packetSize));
            while (packetSize)
            {
                uint8* data = nullptr;
                uint samples = 0;
                DWORD flags = 0;
                uint64 qpctime;
                CHECK(CaptureClient->GetBuffer(&data, &samples, &flags, nullptr, &qpctime));
                double time = (double)qpctime / REFPERSEC;

                uint bytes = samples * BytesPerSample;
                uint pos;
                {
                    ScopeLock lock(RingLock);
                    uint avail = RingSize - (RingWrite - RingRead);
                    if (bytes > avail)
                        RingRead += bytes - avail;

                    RingTimePos = RingWrite;
                    RingTimeValue = time;

                    pos = RingWrite % RingSize;
                    RingWrite += bytes;

                    if (RingRead > RingSize)
                    {
                        RingWrite -= RingSize;
                        RingRead -= RingSize;
                        RingTimePos -= RingSize;
                    }
                }

                uint chunk1 = Min(bytes, RingSize - pos);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                {
                    memset(Ring + pos, 0, chunk1);
                    memset(Ring, 0, bytes - chunk1);
                }
                else
                {
                    memcpy(Ring + pos, data, chunk1);
                    memcpy(Ring, data + chunk1, bytes - chunk1);
                }

                CHECK(CaptureClient->ReleaseBuffer(samples));
                CHECK(CaptureClient->GetNextPacketSize(&packetSize));
            };

        }
    }

public:
    AudioCapture_WASAPI(const CaptureConfig& cfg) : Config(cfg)
    {
        const REFERENCE_TIME duration = REFERENCE_TIME(0.02 * REFPERSEC);

        // init COM
        CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));

        auto& device_info = Devices[cfg.AudioOutputIndex];
        auto device = device_info.Device;
        const bool is_loopback = (device_info.Flow == EDataFlow::eRender); // eRenderならシステム音キャプチャ

        // システム音キャプチャの場合のみ、ダミーの再生クライアントを初期化する
        if (is_loopback)
        {
            // initialize dummy playback client to keep the device running
            WAVEFORMATEX* outFormat = nullptr;
            uint outBufferSize = 0;
            BYTE* outBuffer;
            CHECK(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, PlaybackClient));
            CHECK(PlaybackClient->GetMixFormat(&outFormat));
            CHECK(PlaybackClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, outFormat, NULL));
            CHECK(PlaybackClient->GetBufferSize(&outBufferSize));
            RCPtr<IAudioRenderClient> renderClient;
            CHECK(PlaybackClient->GetService(__uuidof(IAudioRenderClient), renderClient));
            CHECK(renderClient->GetBuffer(outBufferSize, &outBuffer));
            memset(outBuffer, 0, (size_t)outBufferSize * outFormat->nBlockAlign);
            CHECK(PlaybackClient->Start());
        }

        // initialize client for capture (loopback for render, direct for capture)
        CHECK(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, Client));
        CHECK(Client->GetMixFormat((WAVEFORMATEX**)&Format));

        // TODO? support for non-float samples and other channel configs than stereo
        ASSERT(Format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && Format->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        BytesPerSample = Format->Format.nChannels * Format->Format.wBitsPerSample / 8;
        RingSize = Format->Format.nSamplesPerSec * BytesPerSample; // 1 second for now
        Ring = new uint8[RingSize];

        // デバイスの種別に応じてAUDCLNT_STREAMFLAGSを切り替える
        DWORD streamFlags = 0;
        if (is_loopback)
        {
            streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        }

        CHECK(Client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, duration, 0, (WAVEFORMATEX*)Format, NULL));
        CHECK(Client->GetBufferSize(&BufferSize));
        CHECK(Client->GetService(__uuidof(IAudioCaptureClient), CaptureClient));

        // ... and go
        CaptureThread = new Thread(Bind(this, &AudioCapture_WASAPI::CaptureThreadFunc));
        CHECK(Client->Start());
    }

    ~AudioCapture_WASAPI()
    {
        delete CaptureThread;
        Client->Stop();

        // PlaybackClientはシステム音キャプチャの場合にのみ存在する
        // [修正]: 明示的なキャストを使用し、NULLでないことを確認する
        if ((IAudioClient*)PlaybackClient != NULL)
        {
            PlaybackClient->Stop();
            PlaybackClient.Clear();
        }

        CaptureClient.Clear();
        Client.Clear();


        delete[] Ring;

        CoTaskMemFree(Format);
        CoUninitialize();
    }

    AudioInfo GetInfo() const override
    {
        return AudioInfo
        {
            .Format = AudioFormat::F32,
            .Channels = Format->Format.nChannels,
            .SampleRate = Format->Format.nSamplesPerSec,
            .BytesPerSample = BytesPerSample,
        };
    }

    uint Read(uint8* dest, uint size, double& time) override
    {
        ScopeLock lock(RingLock);
        time = RingTimeValue + ((double)RingRead - RingTimePos) / (double)(BytesPerSample * Format->Format.nSamplesPerSec);

        size = Min(size, RingWrite - RingRead);
        uint pos = RingRead % RingSize;
        uint chunk1 = Min(size, RingSize - pos);
        memcpy(dest, Ring + pos, chunk1);
        memcpy(dest + chunk1, Ring, size - chunk1);
        RingRead += size;

        return size;
    }

    void JumpToTime(double time) override
    {
        ScopeLock lock(RingLock);
        int deltasamples = (int)round((time - RingTimeValue) * Format->Format.nSamplesPerSec);
        int destpos = RingTimePos + deltasamples * BytesPerSample;
        //ASSERT(destpos >= (int)RingRead && destpos < (int)RingWrite);
        RingRead = (uint)Clamp<int>(destpos, RingRead, RingWrite);
    }

    void Flush() override
    {
        ScopeLock lock(RingLock);
        RingRead = RingWrite;
    }
};

void InitAudioCapture()
{
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    // Acquire Device enumerator
    RCPtr<IMMDeviceEnumerator> enumerator;
    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator));

    // eRender (出力) デバイスを列挙
    // Get default endpoint (eRender for system audio)
    RCPtr<IMMDevice> defltdev_render;
    CHECK(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defltdev_render));
    // デフォルト出力デバイスをリストに追加
    Devices += AudioDevice{ defltdev_render, EDataFlow::eRender, true };

    // Enumerate all other endpoints (eRender)
    RCPtr<IMMDeviceCollection> collection_render;
    CHECK(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection_render));
    uint count_render = 0;
    CHECK(collection_render->GetCount(&count_render));
    for (uint i = 0; i < count_render; i++)
    {
        RCPtr<IMMDevice> dev;
        CHECK(collection_render->Item(i, dev));
        Devices += AudioDevice{ dev, EDataFlow::eRender, false };
    }

    // eCapture (入力) デバイスを列挙
    // Get default endpoint (eCapture for microphone)
    RCPtr<IMMDevice> defltdev_capture;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, defltdev_capture)))
    {
        // デフォルト入力デバイスをリストに追加
        Devices += AudioDevice{ defltdev_capture, EDataFlow::eCapture, true };
    }

    // Enumerate all other endpoints (eCapture)
    RCPtr<IMMDeviceCollection> collection_capture;
    CHECK(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection_capture));
    uint count_capture = 0;
    CHECK(collection_capture->GetCount(&count_capture));
    for (uint i = 0; i < count_capture; i++)
    {
        RCPtr<IMMDevice> dev;
        CHECK(collection_capture->Item(i, dev));
        Devices += AudioDevice{ dev, EDataFlow::eCapture, false };
    }
}

void GetAudioDevices(Array<String>& into)
{
    into.Clear();

    for (auto& device_info : Devices)
    {
        auto device = device_info.Device;
        EDataFlow flow = device_info.Flow;

        if (device_info.IsDefault)
        {
            // デフォルトデバイスはフレンドリーネームを取得せず、固定名を使用
            if (flow == EDataFlow::eRender)
                into += "Default output (System Sound)";
            else // eCapture
                into += "Default input (Microphone)";

            continue;
        }

        LPWSTR id = nullptr;
        device->GetId(&id);

        RCPtr<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, store)))
        {
            CoTaskMemFree(id);
            continue;
        }

        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (FAILED(store->GetValue(PKEY_Device_FriendlyName, &varName)))
        {
            PropVariantClear(&varName);
            CoTaskMemFree(id);
            continue;
        }

        String prefix = (flow == EDataFlow::eRender) ? "Output: " : "Input: ";
        into += prefix + varName.pwszVal;

        PropVariantClear(&varName);
        CoTaskMemFree(id);
    }
}

IAudioCapture* CreateAudioCaptureWASAPI(const CaptureConfig& config) { return new AudioCapture_WASAPI(config); }