/* audio64: Core Audio (WASAPI) from a 64-bit program, the way games and Chromium use it: the default speakers, an
   event-driven shared stream in the mix format, then a 16-bit mono stream at another rate that the mixer converts.
   "audio64 play" plays a two-second tone. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HRESULT(WINAPI *co_init)(void *, DWORD);
static HRESULT(WINAPI *co_create)(REFCLSID, IUnknown *, DWORD, REFIID, void **);
static void(WINAPI *co_free)(void *);

static const GUID float_format = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};

static double phase;

/* a 440 Hz tone into n frames of the format */
static void tone(BYTE *p, UINT32 n, const WAVEFORMATEX *w, int is_float)
{
    for (UINT32 i = 0; i < n; i++) {
        double v = 0.2 * sin(phase);
        phase += 2 * 3.14159265358979 * 440 / w->nSamplesPerSec;
        for (int c = 0; c < w->nChannels; c++) {
            if (is_float)
                ((float *)p)[i * w->nChannels + c] = (float)v;
            else
                ((short *)p)[i * w->nChannels + c] = (short)(v * 32767);
        }
    }
}

int main(int argc, char **argv)
{
    HMODULE ole = LoadLibraryA("ole32.dll");
    co_init = (void *)GetProcAddress(ole, "CoInitializeEx");
    co_create = (void *)GetProcAddress(ole, "CoCreateInstance");
    co_free = (void *)GetProcAddress(ole, "CoTaskMemFree");
    int seconds_ms = argc > 1 && !strcmp(argv[1], "play") ? 2000 : 400;
    co_init(NULL, 0);
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr = co_create(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) {
        printf("wasapi: no enumerator %lx\n", hr);
        return 1;
    }
    IMMDevice *dev = NULL, *mic = NULL;
    IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    HRESULT mic_hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &mic);
    IMMDeviceCollection *all = NULL;
    UINT count = 0;
    IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &all);
    if (all)
        IMMDeviceCollection_GetCount(all, &count);
    char name[64] = "?";
    IPropertyStore *props = NULL;
    if (dev && SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), NULL, NULL);
        IPropertyStore_Release(props);
    }
    IMMEndpoint *ep = NULL;
    EDataFlow flow = eAll;
    if (dev && SUCCEEDED(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint, (void **)&ep)))
        IMMEndpoint_GetDataFlow(ep, &flow);
    IAudioClient *ac = NULL;
    if (!dev || FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) {
        printf("wasapi: no client\n");
        return 1;
    }
    WAVEFORMATEX *mix = NULL;
    IAudioClient_GetMixFormat(ac, &mix);
    int is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)mix)->SubFormat, &float_format));
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 400000, 0, mix,
                                 NULL);
    HRESULT again = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 400000, 0, mix, NULL);
    UINT32 size = 0;
    IAudioClient_GetBufferSize(ac, &size);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT early = IAudioClient_Start(ac); /* no event yet */
    IAudioClient_SetEventHandle(ac, ev);
    IAudioRenderClient *rc = NULL;
    IAudioClock *clock = NULL;
    ISimpleAudioVolume *vol = NULL;
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clock);
    IAudioClient_GetService(ac, &IID_ISimpleAudioVolume, (void **)&vol);
    float level = 0;
    if (vol) {
        ISimpleAudioVolume_SetMasterVolume(vol, 0.5f, NULL);
        ISimpleAudioVolume_GetMasterVolume(vol, &level);
    }
    if (!rc || FAILED(hr)) {
        printf("wasapi: initialize %lx\n", hr);
        return 1;
    }
    BYTE *data;
    UINT64 written = 0;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, size, &data))) { /* the whole buffer before Start */
        tone(data, size, mix, is_float);
        IAudioRenderClient_ReleaseBuffer(rc, size, 0);
        written += size;
    }
    HRESULT too_big = IAudioRenderClient_GetBuffer(rc, 1, &data); /* full */
    IAudioClient_Start(ac);
    DWORD start = GetTickCount();
    int wakes = 0, timeouts = 0;
    while (GetTickCount() - start < (DWORD)seconds_ms) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) {
            timeouts++;
            continue;
        }
        wakes++;
        UINT32 pad = 0;
        IAudioClient_GetCurrentPadding(ac, &pad);
        UINT32 n = size - pad;
        if (n && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, n, &data))) {
            tone(data, n, mix, is_float);
            IAudioRenderClient_ReleaseBuffer(rc, n, 0);
            written += n;
        }
    }
    DWORD took = GetTickCount() - start;
    UINT64 freq = 0, pos = 0;
    if (clock) {
        IAudioClock_GetFrequency(clock, &freq);
        IAudioClock_GetPosition(clock, &pos, NULL);
    }
    IAudioClient_Stop(ac);
    UINT32 pad = 0;
    IAudioClient_GetCurrentPadding(ac, &pad);
    /* what was released matches the time it took: it was played at the rate, not faster */
    double expect = (double)took * mix->nSamplesPerSec / 1000 + size;
    int paced = written > expect * 0.8 && written < expect * 1.2;
    double played_ms = freq ? (double)pos * 1000 / freq : 0;
    int clocked = played_ms > took * 0.6 && played_ms < took * 1.2 + 50;

    /* a 16-bit mono stream at 22050 Hz, polled */
    IAudioClient *ac2 = NULL;
    IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac2);
    WAVEFORMATEX w16 = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
    HRESULT ok16 = IAudioClient_IsFormatSupported(ac2, AUDCLNT_SHAREMODE_SHARED, &w16, NULL);
    WAVEFORMATEX bad = {0x55 /* MP3 */, 2, 44100, 16000, 1, 0, 0};
    WAVEFORMATEX *closest = NULL;
    HRESULT okbad = IAudioClient_IsFormatSupported(ac2, AUDCLNT_SHAREMODE_SHARED, &bad, &closest);
    if (closest)
        co_free(closest);
    IAudioClient_Initialize(ac2, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, &w16, NULL);
    UINT32 size2 = 0, pad2 = 0, pad3 = 0;
    IAudioClient_GetBufferSize(ac2, &size2);
    IAudioRenderClient *rc2 = NULL;
    IAudioClient_GetService(ac2, &IID_IAudioRenderClient, (void **)&rc2);
    IAudioClient_Start(ac2);
    if (rc2 && SUCCEEDED(IAudioRenderClient_GetBuffer(rc2, size2 / 2, &data))) {
        tone(data, size2 / 2, &w16, 0);
        IAudioRenderClient_ReleaseBuffer(rc2, size2 / 2, 0);
    }
    IAudioClient_GetCurrentPadding(ac2, &pad2);
    Sleep(200);
    IAudioClient_GetCurrentPadding(ac2, &pad3);
    /* 200 ms at 22050 Hz is 4410 frames gone */
    int drained = pad2 > pad3 && pad2 - pad3 > 3000 && pad2 - pad3 < 6500;
    IAudioClient_Stop(ac2);

    printf("wasapi: %s, flow %d, %u device, mic %s, %lu Hz %u ch %s, buffer %s, again %s, early %s, full %s, "
           "volume %.1f, wakes %s, paced %d, clock %d, after stop %s, s16 %s, mp3 %s, resampled %d\n",
           name, (int)flow, count, mic_hr == S_OK ? "yes" : "no", mix->nSamplesPerSec, mix->nChannels,
           is_float ? "float" : "pcm", size >= mix->nSamplesPerSec / 25 ? "ok" : "small",
           again == AUDCLNT_E_ALREADY_INITIALIZED ? "refused" : "?",
           early == AUDCLNT_E_EVENTHANDLE_NOT_SET ? "refused" : "?",
           too_big == AUDCLNT_E_BUFFER_TOO_LARGE ? "refused" : "?",
           level, wakes > took / 20 && timeouts == 0 ? "ok" : "few", paced, clocked, pad > 0 ? "kept" : "empty",
           ok16 == S_OK ? "ok" : "no", okbad == AUDCLNT_E_UNSUPPORTED_FORMAT || okbad == S_FALSE ? "refused" : "?",
           drained);
    if (getenv("AUDIO64_DEBUG"))
        printf("debug: written %llu expect %.0f took %lu size %u pos %llu freq %llu pad2 %u pad3 %u size2 %u\n",
               written, expect, took, size, pos, freq, pad2, pad3, size2);
    if (seconds_ms > 400)
        printf("wasapi: %llu frames in %lu ms, %d wakes\n", written, took, wakes);
    co_free(mix);
    IAudioRenderClient_Release(rc);
    IAudioRenderClient_Release(rc2);
    IAudioClient_Release(ac2);
    IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return 0;
}
