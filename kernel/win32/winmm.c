#include <synth/hal.h>
#include <synth/string.h>

#include "win32.h"

#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER 6
#define JOYERR_UNPLUGGED 167

static UINT WINAPI timeBeginPeriod(UINT period)
{
    UNUSED(period);
    return MMSYSERR_NOERROR;
}

static DWORD WINAPI timeGetTime(void)
{
    return (DWORD)hal_ticks_ms();
}

static UINT WINAPI timeGetDevCaps(uint32_t *caps, UINT size)
{
    if (size < 8)
        return 97;
    caps[0] = 1;
    caps[1] = 1000000;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI timeGetSystemTime(uint32_t *mmt, UINT size)
{
    if (size < 8)
        return 11;
    mmt[0] = 1;
    mmt[1] = (uint32_t)hal_ticks_ms();
    return MMSYSERR_NOERROR;
}

static UINT WINAPI no_devices(void)
{
    return 0;
}

static UINT WINAPI no_driver1(uint32_t a)
{
    UNUSED(a);
    return MMSYSERR_NODRIVER;
}

static UINT WINAPI no_driver2(uint32_t a, uint32_t b)
{
    UNUSED(a);
    UNUSED(b);
    return MMSYSERR_NODRIVER;
}

static UINT WINAPI no_driver3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(c);
    return no_driver2(a, b);
}

static UINT WINAPI bad_device3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    return MMSYSERR_BADDEVICEID;
}

static UINT WINAPI no_device4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return MMSYSERR_BADDEVICEID;
}

static UINT WINAPI no_open5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    return MMSYSERR_BADDEVICEID;
}

static UINT WINAPI no_open6(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    UNUSED(f);
    return no_open5(a, b, c, d, e);
}

static UINT WINAPI joy_unplugged2(uint32_t a, uint32_t b)
{
    UNUSED(a);
    UNUSED(b);
    return JOYERR_UNPLUGGED;
}

static UINT WINAPI joy_unplugged3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(c);
    return joy_unplugged2(a, b);
}

static BOOL WINAPI PlaySound(const void *sound, HANDLE module, DWORD flags)
{
    UNUSED(sound);
    UNUSED(module);
    UNUSED(flags);
    return TRUE;
}

static DWORD WINAPI mciSendString(const void *command, void *ret, UINT len, HANDLE callback)
{
    UNUSED(command);
    UNUSED(callback);
    if (ret && len)
        memset(ret, 0, len);
    return 261;
}

static const struct w32_export winmm_exports[] = {
    W32_FN(timeBeginPeriod),
    W32_AS("timeEndPeriod", timeBeginPeriod),
    W32_FN(timeGetTime),
    W32_FN(timeGetDevCaps),
    W32_FN(timeGetSystemTime),
    W32_AS("timeSetEvent", w32_zero5),
    W32_AS("timeKillEvent", w32_zero1),
    W32_AS("PlaySoundA", PlaySound),
    W32_AS("PlaySoundW", PlaySound),
    W32_AS("sndPlaySoundA", w32_one2),
    W32_AS("sndPlaySoundW", w32_one2),
    W32_AS("mciSendStringA", mciSendString),
    W32_AS("mciSendStringW", mciSendString),
    W32_AS("mciSendCommandA", no_device4),
    W32_AS("mciSendCommandW", no_device4),
    W32_AS("waveOutGetNumDevs", no_devices),
    W32_AS("waveInGetNumDevs", no_devices),
    W32_AS("midiInGetNumDevs", no_devices),
    W32_AS("midiOutGetNumDevs", no_devices),
    W32_AS("auxGetNumDevs", no_devices),
    W32_AS("mixerGetNumDevs", no_devices),
    W32_AS("joyGetNumDevs", no_devices),
    W32_AS("joyGetPos", joy_unplugged2),
    W32_AS("joyGetPosEx", joy_unplugged2),
    W32_AS("joyGetDevCapsA", joy_unplugged3),
    W32_AS("joyGetDevCapsW", joy_unplugged3),
    W32_AS("waveOutOpen", no_open6),
    W32_AS("waveInOpen", no_open6),
    W32_AS("midiOutOpen", no_open5),
    W32_AS("midiInOpen", no_open5),
    W32_AS("mixerOpen", no_open5),
    W32_AS("waveOutGetDevCapsA", bad_device3),
    W32_AS("waveOutGetDevCapsW", bad_device3),
    W32_AS("waveInGetDevCapsA", bad_device3),
    W32_AS("waveInGetDevCapsW", bad_device3),
    W32_AS("midiInGetDevCapsW", bad_device3),
    W32_AS("midiOutGetDevCapsW", bad_device3),
    W32_AS("waveOutClose", no_driver1),
    W32_AS("waveOutReset", no_driver1),
    W32_AS("waveOutPause", no_driver1),
    W32_AS("waveOutRestart", no_driver1),
    W32_AS("waveOutWrite", no_driver3),
    W32_AS("waveOutPrepareHeader", no_driver3),
    W32_AS("waveOutUnprepareHeader", no_driver3),
    W32_AS("waveOutSetVolume", no_driver2),
    W32_AS("waveOutGetVolume", no_driver2),
    W32_AS("waveOutGetPosition", no_driver3),
    W32_AS("midiInClose", no_driver1),
    W32_AS("midiInReset", no_driver1),
    W32_AS("midiInStart", no_driver1),
    W32_AS("midiInStop", no_driver1),
    W32_AS("midiInAddBuffer", no_driver3),
    W32_AS("midiInPrepareHeader", no_driver3),
    W32_AS("midiInUnprepareHeader", no_driver3),
    W32_AS("midiOutClose", no_driver1),
    W32_AS("midiOutReset", no_driver1),
    W32_AS("midiOutShortMsg", no_driver2),
    W32_AS("midiOutLongMsg", no_driver3),
    W32_AS("midiOutPrepareHeader", no_driver3),
    W32_AS("midiOutUnprepareHeader", no_driver3),
};

W32_EXPORTS(winmm_module, "winmm.dll", winmm_exports);
