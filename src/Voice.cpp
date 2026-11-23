// Voice.cpp: squad commands by voice (see Voice.h). One thread of our own runs the Windows speech recognizer (SAPI 5,
// in-process engine, a command grammar of four phrases) on the chosen microphone and reads the same device's peak
// meter (Windows' audio endpoint meter, which reads while the recognizer's capture runs). Nothing here touches the
// game: a command that passes both thresholds goes to Squad::Request, which the main thread carries out.

#include "Voice.h"

#include "Log.h"
#include "Squad.h"

#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <sapi.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>

// The toolchain's endpointvolume.h only declares this interface's name; its methods, in vtable order, as in the
// Windows SDK.
struct IAudioMeterInformation : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* channelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT channelCount, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* hardwareSupport) = 0;
};

namespace SwbfVr
{

namespace Voice
{

namespace
{

/// The recognizer's events and the microphone's level are looked at this often.
constexpr DWORD POLL_MILLISECONDS = 20;
/// Stopping waits this long for the thread to finish.
constexpr DWORD STOP_WAIT_MILLISECONDS = 3000;
/// A summary of the loudest level is logged this often while anything louder than LEVEL_LOG_DECIBELS is heard.
constexpr DWORD LEVEL_LOG_MILLISECONDS = 10000;
constexpr float LEVEL_LOG_DECIBELS = -60.0f;
/// The level of silence (a peak of 0).
constexpr float SILENCE_DECIBELS = -100.0f;
/// The recognizer's language: English (United States), the engine installed with Windows here (MS-1033-80-DESK).
constexpr LANGID ENGLISH_UNITED_STATES = 0x409;
/// Without voice_device: the capture device whose name contains this (Steam Link's headset microphone).
constexpr const char* STEAM_LINK_MICROPHONE = "steam streaming";

/// IAudioMeterInformation {C02216F6-8C67-4B5B-9D00-D008E73E0064} (endpointvolume.h has no uuid declaration for it).
constexpr IID METER_INTERFACE = {0xc02216f6, 0x8c67, 0x4b5b, {0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64}};
/// CLSID_MMDeviceEnumerator {BCDE0395-E52F-467C-8E3D-C4579291692E} (not in the toolchain's libuuid).
constexpr CLSID DEVICE_ENUMERATOR = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
/// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h).
constexpr PROPERTYKEY DEVICE_FRIENDLY_NAME = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

struct Phrase
{
    const wchar_t* text;
    Squad::Command command;
};

const Phrase PHRASES[] = {
    {L"move out", Squad::Command::MoveOut},
    {L"follow me", Squad::Command::FollowMe},
    {L"hold position", Squad::Command::HoldPosition},
    {L"at ease", Squad::Command::AtEase},
};

/// Holds one COM interface and releases it when it goes out of scope (C# would release it in a `using` block).
template <typename Interface>
class ComPointer
{
public:
    ComPointer() = default;
    ComPointer(const ComPointer&) = delete;
    ComPointer& operator=(const ComPointer&) = delete;
    ~ComPointer()
    {
        Reset();
    }
    void Reset()
    {
        if (_pointer)
        {
            _pointer->Release();
            _pointer = nullptr;
        }
    }
    /// For a function that hands out a new interface: releases the held one first.
    Interface** Out()
    {
        Reset();
        return &_pointer;
    }
    void** OutVoid()
    {
        return reinterpret_cast<void**>(Out());
    }
    Interface* Get() const
    {
        return _pointer;
    }
    Interface* operator->() const
    {
        return _pointer;
    }
    explicit operator bool() const
    {
        return _pointer != nullptr;
    }

private:
    Interface* _pointer = nullptr;
};

/// What one run of the listening thread does (owned by the thread).
struct Job
{
    std::wstring file;   // a test WAV file first, if not empty
    std::string device;  // voice_device
    bool isMicrophoneOn; // then the microphone
};

HANDLE _thread = nullptr;
HANDLE _stopEvent = nullptr; // set: the thread ends
bool _isOn = false;
std::string _device;
volatile LONG _minimumConfidencePercent = 60;
volatile LONG _minimumDecibels = -35;

std::string Narrow(const wchar_t* text)
{
    if (!text)
    {
        return std::string();
    }
    char buffer[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
    return buffer;
}

std::string Lower(std::string text)
{
    for (char& letter : text)
    {
        if (letter >= 'A' && letter <= 'Z')
        {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    return text;
}

float ToDecibels(float peak)
{
    return peak > 0.0f ? 20.0f * log10f(peak) : SILENCE_DECIBELS;
}

// Releases what a SAPI event carries (the SDK's SpClearEvent).
void FreeEvent(SPEVENT& event)
{
    if (event.elParamType == SPET_LPARAM_IS_OBJECT || event.elParamType == SPET_LPARAM_IS_TOKEN)
    {
        if (event.lParam)
        {
            reinterpret_cast<IUnknown*>(event.lParam)->Release();
        }
    }
    else if (event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING)
    {
        CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
    }
    event.lParam = 0;
}

// The SAPI audio input whose name contains `wanted` (lower case), else Windows' default. Its name goes to `name`.
bool FindMicrophone(const std::string& wanted, ComPointer<ISpObjectToken>& microphone, std::string* name)
{
    ComPointer<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(
            CLSID_SpObjectTokenCategory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(ISpObjectTokenCategory),
            category.OutVoid())) ||
        FAILED(category->SetId(SPCAT_AUDIOIN, FALSE)))
    {
        return false;
    }
    ComPointer<IEnumSpObjectTokens> tokens;
    if (SUCCEEDED(category->EnumTokens(nullptr, nullptr, tokens.Out())))
    {
        ComPointer<ISpObjectToken> token;
        ULONG fetched = 0;
        while (tokens->Next(1, token.Out(), &fetched) == S_OK && fetched == 1)
        {
            wchar_t* description = nullptr;
            const std::string tokenName =
                SUCCEEDED(token->GetStringValue(nullptr, &description)) ? Narrow(description) : std::string();
            CoTaskMemFree(description);
            Log::Info("voice", "microphone available: %s", tokenName.c_str());
            if (!microphone && Lower(tokenName).find(wanted) != std::string::npos)
            {
                *microphone.Out() = token.Get();
                token.Get()->AddRef();
                *name = tokenName;
            }
        }
    }
    if (microphone)
    {
        return true;
    }
    wchar_t* defaultId = nullptr;
    if (FAILED(category->GetDefaultTokenId(&defaultId)))
    {
        return false;
    }
    const bool isFound = SUCCEEDED(CoCreateInstance(
                             CLSID_SpObjectToken,
                             nullptr,
                             CLSCTX_INPROC_SERVER,
                             __uuidof(ISpObjectToken),
                             microphone.OutVoid())) &&
                         SUCCEEDED(microphone->SetId(nullptr, defaultId, FALSE));
    CoTaskMemFree(defaultId);
    wchar_t* description = nullptr;
    if (isFound && SUCCEEDED(microphone->GetStringValue(nullptr, &description)))
    {
        *name = Narrow(description);
    }
    CoTaskMemFree(description);
    return isFound;
}

// The peak meter of the capture endpoint SAPI's device name belongs to. SAPI's names are the old waveIn names, cut
// to 31 characters, so the endpoint's full name starting with it is a match. Else the default capture endpoint.
void FindMeter(const std::string& microphoneName, ComPointer<IAudioMeterInformation>& meter)
{
    ComPointer<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            DEVICE_ENUMERATOR,
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator),
            enumerator.OutVoid())))
    {
        return;
    }
    const std::string wanted = Lower(microphoneName);
    ComPointer<IMMDeviceCollection> devices;
    UINT count = 0;
    if (!wanted.empty() && SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.Out())) &&
        SUCCEEDED(devices->GetCount(&count)))
    {
        for (UINT index = 0; index < count && !meter; ++index)
        {
            ComPointer<IMMDevice> device;
            ComPointer<IPropertyStore> properties;
            if (FAILED(devices->Item(index, device.Out())) || FAILED(device->OpenPropertyStore(STGM_READ, properties.Out())))
            {
                continue;
            }
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(DEVICE_FRIENDLY_NAME, &value)) && value.vt == VT_LPWSTR &&
                Lower(Narrow(value.pwszVal)).rfind(wanted, 0) == 0)
            {
                device->Activate(METER_INTERFACE, CLSCTX_ALL, nullptr, meter.OutVoid());
            }
            PropVariantClear(&value);
        }
    }
    if (!meter)
    {
        ComPointer<IMMDevice> device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.Out())))
        {
            device->Activate(METER_INTERFACE, CLSCTX_ALL, nullptr, meter.OutVoid());
            Log::Info("voice", "level from the default capture device (no endpoint named like the microphone)");
        }
    }
}

// A phrase the recognizer heard: given to the squad if it passes both thresholds; logged either way.
void Judge(ISpRecoResult* result, bool hasLevel, float decibels)
{
    SPPHRASE* phrase = nullptr;
    const float confidence = SUCCEEDED(result->GetPhrase(&phrase)) && phrase ? phrase->Rule.SREngineConfidence : 0.0f;
    CoTaskMemFree(phrase);
    wchar_t* text = nullptr;
    if (FAILED(result->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) || !text)
    {
        return;
    }
    const Phrase* heard = nullptr;
    for (const Phrase& candidate : PHRASES)
    {
        if (!_wcsicmp(candidate.text, text))
        {
            heard = &candidate;
        }
    }
    const std::string said = Narrow(text);
    CoTaskMemFree(text);
    if (!heard)
    {
        return;
    }
    const int confidencePercent = static_cast<int>(confidence * 100.0f + 0.5f);
    const LONG minimumConfidence = _minimumConfidencePercent, minimumDecibels = _minimumDecibels;
    const char* verdict = confidencePercent < minimumConfidence                                ? "too unsure"
                          : hasLevel && decibels < static_cast<float>(minimumDecibels)          ? "too quiet"
                                                                                                : nullptr;
    Log::Info(
        "voice",
        "heard \"%s\": confidence %d %% (at least %ld), loudest %.0f dB (at least %ld)%s: %s",
        said.c_str(),
        confidencePercent,
        minimumConfidence,
        static_cast<double>(hasLevel ? decibels : 0.0f),
        minimumDecibels,
        hasLevel ? "" : " (not measured)",
        verdict ? verdict : "given");
    if (!verdict)
    {
        Squad::Request(heard->command);
    }
}

// One listening session: a WAV file if `file` is not empty (until it ends), else the microphone (until stopped).
// @return true if a file was finished and the thread may go on to the microphone.
bool Listen(const std::wstring& file, const std::string& device)
{
    const bool isFile = !file.empty();
    ComPointer<ISpRecognizer> recognizer;
    HRESULT result = CoCreateInstance(
        CLSID_SpInprocRecognizer, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ISpRecognizer), recognizer.OutVoid());
    if (FAILED(result))
    {
        Log::Error("voice", "no speech recognizer (0x%08lx): voice commands off", static_cast<unsigned long>(result));
        return false;
    }
    { // the English engine
        ComPointer<ISpObjectTokenCategory> engines;
        ComPointer<IEnumSpObjectTokens> tokens;
        ComPointer<ISpObjectToken> engine;
        ULONG fetched = 0;
        if (FAILED(CoCreateInstance(
                CLSID_SpObjectTokenCategory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                __uuidof(ISpObjectTokenCategory),
                engines.OutVoid())) ||
            FAILED(engines->SetId(SPCAT_RECOGNIZERS, FALSE)) ||
            FAILED(engines->EnumTokens(L"Language=409", nullptr, tokens.Out())) ||
            tokens->Next(1, engine.Out(), &fetched) != S_OK || FAILED(recognizer->SetRecognizer(engine.Get())))
        {
            Log::Error(
                "voice",
                "no English (US) speech recognizer: voice commands off (Windows Settings > Time & language > Speech "
                "can add it)");
            return false;
        }
        wchar_t* description = nullptr;
        if (SUCCEEDED(engine->GetStringValue(nullptr, &description)))
        {
            Log::Info("voice", "recognizer: %s", Narrow(description).c_str());
        }
        CoTaskMemFree(description);
    }
    ComPointer<IAudioMeterInformation> meter;
    if (isFile)
    {
        ComPointer<ISpStream> stream;
        if (FAILED(CoCreateInstance(
                CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ISpStream), stream.OutVoid())) ||
            FAILED(result = stream->BindToFile(file.c_str(), SPFM_OPEN_READONLY, nullptr, nullptr, SPFEI_ALL_EVENTS)) ||
            FAILED(result = recognizer->SetInput(stream.Get(), TRUE)))
        {
            Log::Warning("voice", "test file not opened (0x%08lx)", static_cast<unsigned long>(result));
            return true;
        }
        Log::Info("voice", "listening to the test file %s", Narrow(file.c_str()).c_str());
    }
    else
    {
        ComPointer<ISpObjectToken> microphone;
        std::string microphoneName;
        if (!FindMicrophone(device.empty() ? STEAM_LINK_MICROPHONE : Lower(device), microphone, &microphoneName) ||
            FAILED(result = recognizer->SetInput(microphone.Get(), TRUE)))
        {
            Log::Error(
                "voice",
                "no microphone (0x%08lx): voice commands off. Check Windows Settings > Privacy > Microphone (desktop "
                "apps allowed) and, in the headset, the Steam Link app's microphone permission",
                static_cast<unsigned long>(result));
            return false;
        }
        FindMeter(microphoneName, meter);
        Log::Info(
            "voice",
            "listening to %s (level %s)",
            microphoneName.c_str(),
            meter ? "measured" : "not measured: the loudness threshold is not applied");
    }
    ComPointer<ISpRecoContext> context;
    ComPointer<ISpRecoGrammar> grammar;
    SPSTATEHANDLE rule = nullptr;
    const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_SOUND_START) | SPFEI(SPEI_END_SR_STREAM);
    bool isReady = SUCCEEDED(result = recognizer->CreateRecoContext(context.Out())) &&
                   SUCCEEDED(result = context->SetInterest(interest, interest)) &&
                   SUCCEEDED(result = context->SetNotifyWin32Event()) &&
                   SUCCEEDED(result = context->CreateGrammar(1, grammar.Out())) &&
                   SUCCEEDED(result = grammar->ResetGrammar(ENGLISH_UNITED_STATES)) &&
                   SUCCEEDED(result = grammar->GetRule(L"command", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule));
    for (const Phrase& phrase : PHRASES)
    {
        isReady = isReady && SUCCEEDED(
                                 result = grammar->AddWordTransition(
                                     rule, nullptr, phrase.text, L" ", SPWT_LEXICAL, 1.0f, nullptr));
    }
    isReady = isReady && SUCCEEDED(result = grammar->Commit(0)) &&
              SUCCEEDED(result = grammar->SetRuleState(nullptr, nullptr, SPRS_ACTIVE)) &&
              SUCCEEDED(result = recognizer->SetRecoState(SPRST_ACTIVE));
    if (!isReady)
    {
        Log::Error(
            "voice",
            "recognizer not started (0x%08lx): voice commands off. If this says 0x80070005 (access denied), allow "
            "desktop apps the microphone in Windows Settings > Privacy",
            static_cast<unsigned long>(result));
        return false;
    }
    const HANDLE waits[2] = {_stopEvent, context->GetNotifyEventHandle()};
    float peakSinceSound = SILENCE_DECIBELS, loudestInPeriod = SILENCE_DECIBELS;
    DWORD periodStart = GetTickCount();
    for (;;)
    {
        if (WaitForMultipleObjects(2, waits, FALSE, POLL_MILLISECONDS) == WAIT_OBJECT_0)
        {
            return false;
        }
        float peak = 0.0f;
        const float decibels = meter && SUCCEEDED(meter->GetPeakValue(&peak)) ? ToDecibels(peak) : SILENCE_DECIBELS;
        peakSinceSound = decibels > peakSinceSound ? decibels : peakSinceSound;
        loudestInPeriod = decibels > loudestInPeriod ? decibels : loudestInPeriod;
        bool isFileDone = false;
        SPEVENT event = {};
        ULONG fetched = 0;
        while (context->GetEvents(1, &event, &fetched) == S_OK && fetched == 1)
        {
            if (event.eEventId == SPEI_SOUND_START)
            {
                peakSinceSound = decibels; // the loudest moment of what is said from here on
            }
            else if (event.eEventId == SPEI_RECOGNITION && event.elParamType == SPET_LPARAM_IS_OBJECT)
            {
                Judge(reinterpret_cast<ISpRecoResult*>(event.lParam), meter && !isFile, peakSinceSound);
            }
            else if (event.eEventId == SPEI_END_SR_STREAM && isFile)
            {
                isFileDone = true;
            }
            FreeEvent(event);
        }
        if (isFileDone)
        {
            Log::Info("voice", "test file finished");
            return true;
        }
        const DWORD now = GetTickCount();
        if (now - periodStart >= LEVEL_LOG_MILLISECONDS)
        {
            if (meter && loudestInPeriod > LEVEL_LOG_DECIBELS)
            {
                Log::Info(
                    "voice",
                    "loudest %.0f dB in the last %lu s (threshold %ld dB)",
                    static_cast<double>(loudestInPeriod),
                    static_cast<unsigned long>(LEVEL_LOG_MILLISECONDS / 1000),
                    static_cast<long>(_minimumDecibels));
            }
            loudestInPeriod = SILENCE_DECIBELS;
            periodStart = now;
        }
    }
}

DWORD WINAPI ListenThread(void* parameter)
{
    const std::unique_ptr<Job> job(static_cast<Job*>(parameter));
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        Log::Error("voice", "COM not available on the voice thread: voice commands off");
        return 0;
    }
    const bool shouldGoOn = job->file.empty() || Listen(job->file, job->device);
    if (shouldGoOn && job->isMicrophoneOn)
    {
        Listen(std::wstring(), job->device);
    }
    CoUninitialize();
    return 0;
}

void Stop()
{
    if (!_thread)
    {
        return;
    }
    SetEvent(_stopEvent);
    if (WaitForSingleObject(_thread, STOP_WAIT_MILLISECONDS) != WAIT_OBJECT_0)
    {
        Log::Warning("voice", "the listening thread did not stop in time");
    }
    CloseHandle(_thread);
    _thread = nullptr;
    ResetEvent(_stopEvent);
}

void Start(const std::wstring& file)
{
    if (!_stopEvent)
    {
        _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    Job* const job = new Job{file, _device, _isOn};
    _thread = CreateThread(nullptr, 0, ListenThread, job, 0, nullptr);
    if (!_thread)
    {
        delete job;
        Log::Error("voice", "listening thread not started: voice commands off");
    }
}

} // namespace

void SetOptions(bool isOn, int minimumConfidencePercent, int minimumDecibels, const char* deviceName)
{
    InterlockedExchange(&_minimumConfidencePercent, minimumConfidencePercent);
    InterlockedExchange(&_minimumDecibels, minimumDecibels);
    const std::string device = deviceName ? deviceName : "";
    const bool isRunning = _thread && WaitForSingleObject(_thread, 0) == WAIT_TIMEOUT;
    if (isOn == _isOn && device == _device && isRunning == isOn)
    {
        return;
    }
    _isOn = isOn;
    _device = device;
    Stop();
    Log::Info(
        "voice",
        "voice commands %s (confidence at least %d %%, loudness at least %d dB)",
        isOn ? "on" : "off",
        minimumConfidencePercent,
        minimumDecibels);
    if (isOn)
    {
        Start(std::wstring());
    }
}

void ListenToFile(const char* path)
{
    wchar_t widePath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, MAX_PATH - 1);
    Stop();
    Start(widePath);
}

} // namespace Voice

} // namespace SwbfVr
