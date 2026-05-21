// audiostream.cpp - Audio streamer over TCP
// Sender: captures from VB-CABLE virtual cable, streams over TCP
// Receiver: receives TCP stream, plays to audio output
//
// Build:
//   cl /EHsc audiostream.cpp /link ws2_32.lib ole32.lib oleaut32.lib
//
// Usage:
//   audiostream.exe --mode sender --port 8888 --device CABLE
//   audiostream.exe --mode receiver --ip 192.168.0.1 --port 8888

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

// Local GUIDs to avoid MinGW link issues with DEFINE_GUID symbols
static const GUID GUID_IEEE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ======================================================================
// Constants
// ======================================================================
static const UINT32 NET_SYNC_WORD = 0x41554449; // "AUDI" as sync marker

// ======================================================================
// Configuration
// ======================================================================
struct Config {
    std::string mode = "sender";
    std::string ip = "0.0.0.0";
    int port = 8888;
    std::string device_name = "CABLE";
    int sample_rate = 48000;
    int channels = 2;
    int bits_per_sample = 16;
    int buffer_ms = 50;
    bool list_devices = false;
};

static void PrintUsage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --mode <sender|receiver>     Run mode (default: sender)\n");
    printf("  --ip <address>\n");
    printf("       Sender: bind address (default: 0.0.0.0)\n");
    printf("       Receiver: server IP to connect to\n");
    printf("  --port <port>                TCP port (default: 8888)\n");
    printf("  --device <name>              Audio device name substring match\n");
    printf("       Sender: capture device name (default: CABLE)\n");
    printf("       Receiver: output device name (empty=default)\n");
    printf("  --samplerate <hz>            Sample rate (default: 48000)\n");
    printf("  --channels <n>               Channel count (default: 2)\n");
    printf("  --bits <n>                   Bits per sample (default: 16)\n");
    printf("  --buffer <ms>                Buffer time in ms (default: 50)\n");
    printf("  --list-devices               List audio devices and exit\n");
    printf("  --help                       Show this help\n");
}

static bool ParseArgs(int argc, char* argv[], Config& cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            exit(0);
        } else if (arg == "--list-devices") {
            cfg.list_devices = true;
        } else if (arg == "--mode") {
            if (++i >= argc) { printf("--mode requires argument\n"); return false; }
            cfg.mode = argv[i];
        } else if (arg == "--ip") {
            if (++i >= argc) { printf("--ip requires argument\n"); return false; }
            cfg.ip = argv[i];
        } else if (arg == "--port") {
            if (++i >= argc) { printf("--port requires argument\n"); return false; }
            cfg.port = atoi(argv[i]);
            if (cfg.port <= 0 || cfg.port > 65535) {
                printf("Invalid port: %d\n", cfg.port); return false;
            }
        } else if (arg == "--device") {
            if (++i >= argc) { printf("--device requires argument\n"); return false; }
            cfg.device_name = argv[i];
        } else if (arg == "--samplerate") {
            if (++i >= argc) { printf("--samplerate requires argument\n"); return false; }
            cfg.sample_rate = atoi(argv[i]);
            if (cfg.sample_rate <= 0) {
                printf("Invalid sample rate\n"); return false;
            }
        } else if (arg == "--channels") {
            if (++i >= argc) { printf("--channels requires argument\n"); return false; }
            cfg.channels = atoi(argv[i]);
            if (cfg.channels <= 0 || cfg.channels > 8) {
                printf("Invalid channels\n"); return false;
            }
        } else if (arg == "--bits") {
            if (++i >= argc) { printf("--bits requires argument\n"); return false; }
            cfg.bits_per_sample = atoi(argv[i]);
            if (cfg.bits_per_sample % 8 != 0 || cfg.bits_per_sample > 32) {
                printf("Bits per sample must be 8, 16, 24, or 32\n"); return false;
            }
        } else if (arg == "--buffer") {
            if (++i >= argc) { printf("--buffer requires argument\n"); return false; }
            cfg.buffer_ms = atoi(argv[i]);
            if (cfg.buffer_ms <= 0) {
                printf("Invalid buffer time\n"); return false;
            }
        } else {
            printf("Unknown option: %s\n", arg.c_str());
            return false;
        }
    }
    // Mode validation
    if (cfg.mode != "sender" && cfg.mode != "receiver") {
        printf("Mode must be 'sender' or 'receiver'\n"); return false;
    }
    return true;
}

// ======================================================================
// Audio device helpers
// ======================================================================
static HRESULT GetDeviceFriendlyName(IMMDevice* device, std::wstring& name) {
    IPropertyStore* props = NULL;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) return hr;
    PROPVARIANT var;
    PropVariantInit(&var);
    hr = props->GetValue(PKEY_Device_FriendlyName, &var);
    if (SUCCEEDED(hr) && var.pwszVal) name = var.pwszVal;
    PropVariantClear(&var);
    props->Release();
    return hr;
}

static void ListAudioDevices() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("COM init failed\n"); return; }
    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { printf("Failed to create enumerator\n"); CoUninitialize(); return; }
    printf("\n=== Capture Devices (Input) ===\n");
    IMMDeviceCollection* col = NULL;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT c = 0; col->GetCount(&c);
        for (UINT i = 0; i < c; i++) {
            IMMDevice* d = NULL; col->Item(i, &d);
            std::wstring n; GetDeviceFriendlyName(d, n);
            printf("  [%u] %ls\n", i, n.c_str()); d->Release();
        }
        col->Release();
    }
    printf("\n=== Render Devices (Output) ===\n");
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT c = 0; col->GetCount(&c);
        for (UINT i = 0; i < c; i++) {
            IMMDevice* d = NULL; col->Item(i, &d);
            std::wstring n; GetDeviceFriendlyName(d, n);
            printf("  [%u] %ls\n", i, n.c_str()); d->Release();
        }
        col->Release();
    }
    enumerator->Release();
    CoUninitialize();
}

static HRESULT FindCaptureDevice(IMMDeviceEnumerator* enumerator,
                                  const std::string& substr,
                                  IMMDevice** result) {
    if (substr.empty()) {
        // Use default capture device
        return enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, result);
    }
    IMMDeviceCollection* col = NULL;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) return hr;
    UINT c = 0; col->GetCount(&c);
    std::wstring wsub(substr.begin(), substr.end());
    *result = NULL;
    for (UINT i = 0; i < c; i++) {
        IMMDevice* d = NULL; col->Item(i, &d);
        std::wstring n; GetDeviceFriendlyName(d, n);
        if (n.find(wsub) != std::wstring::npos) { *result = d; break; }
        d->Release();
    }
    col->Release();
    return (*result) ? S_OK : E_NOTFOUND;
}

static HRESULT FindRenderDevice(IMMDeviceEnumerator* enumerator,
                                 const std::string& substr,
                                 IMMDevice** result) {
    if (substr.empty()) {
        return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, result);
    }
    IMMDeviceCollection* col = NULL;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) return hr;
    UINT c = 0; col->GetCount(&c);
    std::wstring wsub(substr.begin(), substr.end());
    *result = NULL;
    for (UINT i = 0; i < c; i++) {
        IMMDevice* d = NULL; col->Item(i, &d);
        std::wstring n; GetDeviceFriendlyName(d, n);
        if (n.find(wsub) != std::wstring::npos) { *result = d; break; }
        d->Release();
    }
    col->Release();
    return (*result) ? S_OK : E_NOTFOUND;
}

// ======================================================================
// Format conversion helpers
// ======================================================================
// Convert IEEE float (range -1..1) to signed 16-bit PCM
static void Float32ToInt16(const float* input, int16_t* output, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        float s = input[i];
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        output[i] = (int16_t)(s * 32767.0f);
    }
}

// Convert 32-bit PCM to 16-bit PCM
static void Int32ToInt16(const int32_t* input, int16_t* output, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        int32_t s = input[i] >> 16;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        output[i] = (int16_t)s;
    }
}

// ======================================================================
// Shared ring buffer (receiver: network thread -> audio render)
// Uses modulo (not bitmask) so capacity doesn't need to be power of 2
// ======================================================================
class RingBuffer {
public:
    RingBuffer(size_t capacity_bytes)
        : buf(capacity_bytes, 0), cap(capacity_bytes), rp(0), wp(0) {
        InitializeCriticalSection(&cs);
    }
    ~RingBuffer() { DeleteCriticalSection(&cs); }

    size_t Readable() const {
        return (wp + cap - rp) % cap;
    }
    size_t Writable() const {
        return cap - Readable() - 1;
    }

    size_t Write(const uint8_t* data, size_t bytes) {
        EnterCriticalSection(&cs);
        size_t avail = Writable();
        if (bytes > avail) bytes = avail;
        if (bytes == 0) { LeaveCriticalSection(&cs); return 0; }
        size_t first = (std::min)(bytes, cap - wp);
        memcpy(&buf[wp], data, first);
        if (bytes > first) memcpy(&buf[0], data + first, bytes - first);
        wp = (wp + bytes) % cap;
        LeaveCriticalSection(&cs);
        return bytes;
    }

    size_t Read(uint8_t* data, size_t bytes) {
        EnterCriticalSection(&cs);
        size_t avail = Readable();
        if (bytes > avail) bytes = avail;
        if (bytes == 0) { LeaveCriticalSection(&cs); return 0; }
        size_t first = (std::min)(bytes, cap - rp);
        memcpy(data, &buf[rp], first);
        if (bytes > first) memcpy(data + first, &buf[0], bytes - first);
        rp = (rp + bytes) % cap;
        LeaveCriticalSection(&cs);
        return bytes;
    }

    void Reset() { EnterCriticalSection(&cs); rp = wp = 0; LeaveCriticalSection(&cs); }

private:
    std::vector<uint8_t> buf;
    size_t cap, rp, wp;
    CRITICAL_SECTION cs;
};

// ======================================================================
// Console Ctrl-C handler
// ======================================================================
static std::atomic<bool> g_running(true);
static BOOL WINAPI CtrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

// ======================================================================
// Sender: capture from VBCABLE, send over TCP
// ======================================================================
static int RunSender(const Config& cfg) {
    printf("=== Sender ===\n");
    printf("Device:    %s\n", cfg.device_name.c_str());
    printf("Format:    %d Hz, %d ch, %d-bit PCM\n", cfg.sample_rate, cfg.channels, cfg.bits_per_sample);
    printf("Buffer:    %d ms\n", cfg.buffer_ms);
    printf("Listen:    %s:%d\n", cfg.ip.c_str(), cfg.port);
    printf("Waiting for receiver to connect...\n\n");

    // ---- COM ----
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx failed: 0x%08X\n", hr); return 1; }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { printf("MMDeviceEnumerator failed: 0x%08X\n", hr); CoUninitialize(); return 1; }

    IMMDevice* captureDevice = NULL;
    hr = FindCaptureDevice(enumerator, cfg.device_name, &captureDevice);
    if (FAILED(hr)) {
        printf("Capture device '%s' not found. Use --list-devices to see available devices.\n",
               cfg.device_name.c_str());
        enumerator->Release(); CoUninitialize(); return 1;
    }
    std::wstring devName;
    GetDeviceFriendlyName(captureDevice, devName);
    printf("Capture device: %ls\n", devName.c_str());

    // ---- WASAPI capture ----
    IAudioClient* audioClient = NULL;
    hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                  (void**)&audioClient);
    if (FAILED(hr)) { printf("Activate IAudioClient failed: 0x%08X\n", hr);
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    // Get mix format
    WAVEFORMATEX* mixFormat = NULL;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) { printf("GetMixFormat failed: 0x%08X\n", hr);
        audioClient->Release(); captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }
    printf("Mix format: %d Hz, %d ch, %d-bit (tag=0x%04X)\n",
           mixFormat->nSamplesPerSec, mixFormat->nChannels,
           mixFormat->wBitsPerSample, mixFormat->wFormatTag);

    // Build requested format (PCM16)
    WAVEFORMATEX requestedFormat = {0};
    requestedFormat.wFormatTag = WAVE_FORMAT_PCM;
    requestedFormat.nChannels = (WORD)cfg.channels;
    requestedFormat.nSamplesPerSec = cfg.sample_rate;
    requestedFormat.wBitsPerSample = (WORD)cfg.bits_per_sample;
    requestedFormat.nBlockAlign = (requestedFormat.nChannels * requestedFormat.wBitsPerSample) / 8;
    requestedFormat.nAvgBytesPerSec = requestedFormat.nSamplesPerSec * requestedFormat.nBlockAlign;
    requestedFormat.cbSize = 0;

    // Try PCM16 first
    WAVEFORMATEX* useFormat = &requestedFormat;
    bool needConvert = false;
    WAVEFORMATEX* closestMatch = NULL;
    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &requestedFormat, &closestMatch);
    if (FAILED(hr)) {
        printf("PCM16 not supported in shared mode (0x%08X). Using mix format.\n", hr);
        useFormat = mixFormat;
        // Check if conversion needed
        needConvert = (useFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       useFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    }

    REFERENCE_TIME bufDuration = (REFERENCE_TIME)cfg.buffer_ms * 10000; // ms -> 100ns
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  bufDuration, 0, useFormat, NULL);
    if (FAILED(hr)) {
        printf("Initialize failed (0x%08X). Trying default buffer duration...\n", hr);
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                      0, 0, useFormat, NULL);
        if (FAILED(hr)) {
            printf("AudioClient::Initialize failed: 0x%08X\n", hr);
            CoTaskMemFree(mixFormat); audioClient->Release();
            captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
        }
    }

    UINT32 frameSize = useFormat->nBlockAlign;
    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);
    printf("WASAPI buffer: %u frames (~%d ms)\n", bufferFrameCount,
           (int)(bufferFrameCount * 1000 / useFormat->nSamplesPerSec));

    HANDLE captureEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!captureEvent) { printf("CreateEvent failed\n");
        CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }
    hr = audioClient->SetEventHandle(captureEvent);
    if (FAILED(hr)) { printf("SetEventHandle failed: 0x%08X\n", hr);
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    IAudioCaptureClient* captureClient = NULL;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) { printf("GetService(IAudioCaptureClient) failed: 0x%08X\n", hr);
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    // ---- Winsock ----
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n"); captureClient->Release(); CloseHandle(captureEvent);
        CoTaskMemFree(mixFormat); audioClient->Release(); captureDevice->Release();
        enumerator->Release(); CoUninitialize(); return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup(); captureClient->Release(); CloseHandle(captureEvent);
        CoTaskMemFree(mixFormat); audioClient->Release(); captureDevice->Release();
        enumerator->Release(); CoUninitialize(); return 1;
    }
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in bindAddr = {0};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons((u_short)cfg.port);
    inet_pton(AF_INET, cfg.ip.c_str(), &bindAddr.sin_addr);

    if (bind(listenSock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("bind() failed: %d\n", WSAGetLastError());
        closesocket(listenSock); WSACleanup(); captureClient->Release();
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }
    if (listen(listenSock, 1) == SOCKET_ERROR) {
        printf("listen() failed: %d\n", WSAGetLastError());
        closesocket(listenSock); WSACleanup(); captureClient->Release();
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }

    // Accept one receiver
    sockaddr_in clientAddr = {0};
    int addrLen = sizeof(clientAddr);
    SOCKET dataSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
    if (dataSock == INVALID_SOCKET) {
        printf("accept() failed: %d\n", WSAGetLastError());
        closesocket(listenSock); WSACleanup(); captureClient->Release();
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }
    char clientIP[64] = {0};
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
    printf("Receiver connected from %s:%d\n", clientIP, ntohs(clientAddr.sin_port));
    closesocket(listenSock); // no more connections needed

    // TCP_NODELAY for lower latency
    opt = 1;
    setsockopt(dataSock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));

    // ---- Start capture ----
    hr = audioClient->Start();
    if (FAILED(hr)) { printf("AudioClient::Start failed: 0x%08X\n", hr);
        closesocket(dataSock); WSACleanup(); captureClient->Release();
        CloseHandle(captureEvent); CoTaskMemFree(mixFormat); audioClient->Release();
        captureDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    printf("Streaming started. Press Ctrl+C to stop.\n\n");

    // ---- Capture & send loop ----
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Pre-allocate conversion buffer if needed
    std::vector<uint8_t> convertBuf;
    if (needConvert) {
        // Max frames we might get in one packet
        convertBuf.resize(bufferFrameCount * sizeof(int16_t) * cfg.channels);
    }

    auto startTime = std::chrono::steady_clock::now();
    uint64_t totalBytesSent = 0;
    uint64_t totalFrames = 0;

    while (g_running) {
        DWORD waitRes = WaitForSingleObject(captureEvent, 200);
        if (waitRes == WAIT_TIMEOUT) continue;
        if (waitRes != WAIT_OBJECT_0) {
            printf("WaitForSingleObject failed: %d\n", waitRes);
            break;
        }
        UINT32 packetLen = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetLen)) && packetLen > 0) {
            BYTE* data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &frames, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            if (frames > 0) {
                const BYTE* sendData = data;
                UINT32 sendBytes = frames * frameSize;

                // Silence buffer (lives through the entire loop iteration)
                std::vector<uint8_t> silenceBuf;

                // Convert if mix format differs from requested PCM16
                if (needConvert) {
                    // Convert from mix format to PCM16
                    // (silence data is handled the same way - zeros stay zeros)
                    UINT32 samples = frames * cfg.channels;
                    if (useFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (useFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         ((WAVEFORMATEXTENSIBLE*)useFormat)->SubFormat == GUID_IEEE_FLOAT)) {
                        // Float -> PCM16
                        convertBuf.resize(samples * sizeof(int16_t));
                        Float32ToInt16((const float*)data, (int16_t*)convertBuf.data(), samples);
                        sendData = convertBuf.data();
                        sendBytes = (UINT32)(samples * sizeof(int16_t));
                    } else if (useFormat->wBitsPerSample == 32 &&
                               useFormat->wFormatTag == WAVE_FORMAT_PCM) {
                        // 32-bit PCM -> PCM16
                        convertBuf.resize(samples * sizeof(int16_t));
                        Int32ToInt16((const int32_t*)data, (int16_t*)convertBuf.data(), samples);
                        sendData = convertBuf.data();
                        sendBytes = (UINT32)(samples * sizeof(int16_t));
                    } else {
                        // Unknown format, just send raw
                        convertBuf.resize(sendBytes);
                        memcpy(convertBuf.data(), data, sendBytes);
                        sendData = convertBuf.data();
                    }
                } else if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Silent data in PCM16 mode - send zeros
                    sendBytes = frames * frameSize;
                    silenceBuf.assign(sendBytes, 0);
                    sendData = silenceBuf.data();
                }

                // Send size prefix + audio data
                uint32_t netSize = sendBytes;
                int ret = send(dataSock, (char*)&netSize, sizeof(netSize), 0);
                if (ret == SOCKET_ERROR) {
                    printf("Send failed (receiver disconnected): %d\n", WSAGetLastError());
                    g_running = false;
                    captureClient->ReleaseBuffer(frames);
                    break;
                }
                ret = send(dataSock, (const char*)sendData, sendBytes, 0);
                if (ret == SOCKET_ERROR) {
                    printf("Send failed (receiver disconnected): %d\n", WSAGetLastError());
                    g_running = false;
                    captureClient->ReleaseBuffer(frames);
                    break;
                }
                totalBytesSent += sendBytes;
                totalFrames += frames;
            }
            captureClient->ReleaseBuffer(frames);
        }
    }

    audioClient->Stop();

    // ---- Stats ----
    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    if (elapsed > 0 && totalFrames > 0) {
        double actualRate = (double)totalFrames / elapsed * 1000.0;
        printf("\n--- Sender Stats ---\n");
        printf("Duration:    %lld ms\n", elapsed);
        printf("Frames sent: %llu\n", totalFrames);
        printf("Bytes sent:  %llu\n", totalBytesSent);
        printf("Avg rate:    %.0f Hz (%.2f Kbps)\n", actualRate,
               totalBytesSent * 8.0 / elapsed);
    }

    // ---- Cleanup ----
    closesocket(dataSock);
    WSACleanup();
    captureClient->Release();
    CloseHandle(captureEvent);
    CoTaskMemFree(mixFormat);
    audioClient->Release();
    captureDevice->Release();
    enumerator->Release();
    CoUninitialize();
    printf("Sender stopped.\n");
    return 0;
}

// ======================================================================
// Receiver network thread
// ======================================================================
struct RecvThreadCtx {
    SOCKET sock;
    RingBuffer* ring;
    std::atomic<bool>* running;
};

static DWORD WINAPI RecvNetworkThread(LPVOID param) {
    RecvThreadCtx* ctx = (RecvThreadCtx*)param;
    RingBuffer* ring = ctx->ring;

    while (*ctx->running) {
        uint32_t dataSize = 0;
        int ret = recv(ctx->sock, (char*)&dataSize, sizeof(dataSize), MSG_WAITALL);
        if (ret <= 0) {
            if (ret == 0) printf("Server disconnected.\n");
            else printf("recv() header failed: %d\n", WSAGetLastError());
            *ctx->running = false;
            break;
        }
        // Sanity check
        if (dataSize == 0 || dataSize > 1024 * 1024) { // max 1MB per packet
            printf("Invalid packet size: %u\n", dataSize);
            *ctx->running = false;
            break;
        }
        // Read audio data in chunks to avoid oversized stack buffer
        uint32_t remaining = dataSize;
        while (remaining > 0) {
            uint8_t temp[8192];
            uint32_t toRead = (remaining > sizeof(temp)) ? (uint32_t)sizeof(temp) : remaining;
            ret = recv(ctx->sock, (char*)temp, toRead, MSG_WAITALL);
            if (ret <= 0) {
                printf("recv() data failed: %d\n", WSAGetLastError());
                *ctx->running = false;
                break;
            }
            ring->Write(temp, ret);
            remaining -= ret;
        }
    }
    return 0;
}

// ======================================================================
// Receiver: receive from TCP, play audio
// ======================================================================
static int RunReceiver(const Config& cfg) {
    printf("=== Receiver ===\n");
    printf("Server:    %s:%d\n", cfg.ip.c_str(), cfg.port);
    if (!cfg.device_name.empty())
        printf("Device:    %s\n", cfg.device_name.c_str());
    else
        printf("Device:    (default)\n");
    printf("Format:    %d Hz, %d ch, %d-bit PCM\n", cfg.sample_rate, cfg.channels, cfg.bits_per_sample);
    printf("Buffer:    %d ms\n", cfg.buffer_ms);
    printf("Connecting...\n\n");

    // ---- COM ----
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx failed: 0x%08X\n", hr); return 1; }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { printf("MMDeviceEnumerator failed: 0x%08X\n", hr); CoUninitialize(); return 1; }

    IMMDevice* renderDevice = NULL;
    hr = FindRenderDevice(enumerator, cfg.device_name, &renderDevice);
    if (FAILED(hr)) {
        printf("Render device not found. Use --list-devices.\n");
        enumerator->Release(); CoUninitialize(); return 1;
    }
    std::wstring devName;
    GetDeviceFriendlyName(renderDevice, devName);
    printf("Render device: %ls\n", devName.c_str());

    // ---- WASAPI render ----
    IAudioClient* audioClient = NULL;
    hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                 (void**)&audioClient);
    if (FAILED(hr)) { printf("Activate IAudioClient failed: 0x%08X\n", hr);
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    WAVEFORMATEX* mixFormat = NULL;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) { printf("GetMixFormat failed: 0x%08X\n", hr);
        audioClient->Release(); renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }
    printf("Output mix format: %d Hz, %d ch, %d-bit\n",
           mixFormat->nSamplesPerSec, mixFormat->nChannels, mixFormat->wBitsPerSample);

    // Use PCM16 format (same as sender)
    WAVEFORMATEX pcmFormat = {0};
    pcmFormat.wFormatTag = WAVE_FORMAT_PCM;
    pcmFormat.nChannels = (WORD)cfg.channels;
    pcmFormat.nSamplesPerSec = cfg.sample_rate;
    pcmFormat.wBitsPerSample = (WORD)cfg.bits_per_sample;
    pcmFormat.nBlockAlign = (pcmFormat.nChannels * pcmFormat.wBitsPerSample) / 8;
    pcmFormat.nAvgBytesPerSec = pcmFormat.nSamplesPerSec * pcmFormat.nBlockAlign;
    pcmFormat.cbSize = 0;

    REFERENCE_TIME bufDuration = (REFERENCE_TIME)cfg.buffer_ms * 10000;
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  bufDuration, 0, &pcmFormat, NULL);
    if (FAILED(hr)) {
        // Try default buffer size
        printf("Initialize failed (0x%08X). Trying default buffer...\n", hr);
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                      0, 0, &pcmFormat, NULL);
    }
    if (FAILED(hr)) {
        printf("AudioClient::Initialize failed: 0x%08X\n", hr);
        printf("PCM16 format not supported on this device.\n");
        CoTaskMemFree(mixFormat); audioClient->Release();
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }

    UINT32 frameSize = pcmFormat.nBlockAlign;
    UINT32 bufferFrames = 0;
    audioClient->GetBufferSize(&bufferFrames);
    printf("WASAPI buffer: %u frames (~%d ms)\n", bufferFrames,
           (int)(bufferFrames * 1000 / pcmFormat.nSamplesPerSec));

    IAudioRenderClient* renderClient = NULL;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) { printf("GetService(IAudioRenderClient) failed: 0x%08X\n", hr);
        CoTaskMemFree(mixFormat); audioClient->Release();
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    HANDLE renderEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!renderEvent) { printf("CreateEvent failed\n");
        renderClient->Release(); CoTaskMemFree(mixFormat); audioClient->Release();
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }
    hr = audioClient->SetEventHandle(renderEvent);
    if (FAILED(hr)) { printf("SetEventHandle failed: 0x%08X\n", hr);
        CloseHandle(renderEvent); renderClient->Release(); CoTaskMemFree(mixFormat);
        audioClient->Release(); renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    // ---- Winsock & connect ----
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        CloseHandle(renderEvent); renderClient->Release(); CoTaskMemFree(mixFormat);
        audioClient->Release(); renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }
    SOCKET dataSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (dataSock == INVALID_SOCKET) { printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup(); CloseHandle(renderEvent); renderClient->Release(); CoTaskMemFree(mixFormat);
        audioClient->Release(); renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    sockaddr_in srvAddr = {0};
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons((u_short)cfg.port);
    inet_pton(AF_INET, cfg.ip.c_str(), &srvAddr.sin_addr);

    if (connect(dataSock, (sockaddr*)&srvAddr, sizeof(srvAddr)) == SOCKET_ERROR) {
        printf("connect() to %s:%d failed: %d\n", cfg.ip.c_str(), cfg.port, WSAGetLastError());
        closesocket(dataSock); WSACleanup(); CloseHandle(renderEvent);
        renderClient->Release(); CoTaskMemFree(mixFormat); audioClient->Release();
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1;
    }
    printf("Connected to sender.\n");

    int opt = 1;
    setsockopt(dataSock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));

    // ---- Ring buffer (holds ~2 seconds of audio) ----
    size_t ringCapacity = (size_t)pcmFormat.nAvgBytesPerSec * 2;
    RingBuffer ring(ringCapacity);
    RecvThreadCtx threadCtx = { dataSock, &ring, &g_running };
    HANDLE recvThread = CreateThread(NULL, 0, RecvNetworkThread, &threadCtx, 0, NULL);
    if (!recvThread) { printf("CreateThread failed\n");
        closesocket(dataSock); WSACleanup(); CloseHandle(renderEvent);
        renderClient->Release(); CoTaskMemFree(mixFormat); audioClient->Release();
        renderDevice->Release(); enumerator->Release(); CoUninitialize(); return 1; }

    // ---- Pre-fill buffer ----
    UINT32 prefillFrames = (UINT32)(cfg.buffer_ms * pcmFormat.nSamplesPerSec / 1000);
    if (prefillFrames > bufferFrames) prefillFrames = bufferFrames;

    printf("Pre-filling %u frames (%d ms)...\n", prefillFrames, cfg.buffer_ms);

    // Wait for enough data from network
    size_t prefillBytes = prefillFrames * frameSize;
    auto prefillStart = std::chrono::steady_clock::now();
    while (g_running) {
        if (ring.Readable() >= prefillBytes) break;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - prefillStart).count() > 30) {
            printf("Timeout waiting for audio data from sender.\n");
            g_running = false;
            break;
        }
        Sleep(10);
    }

    if (g_running) {
        // Write prefill data to WASAPI buffer
        BYTE* wasapiBuf = NULL;
        hr = renderClient->GetBuffer(prefillFrames, &wasapiBuf);
        if (SUCCEEDED(hr)) {
            size_t got = ring.Read(wasapiBuf, prefillBytes);
            if (got < prefillBytes) {
                memset(wasapiBuf + got, 0, prefillBytes - got);
            }
            renderClient->ReleaseBuffer(prefillFrames, 0);
        }

        hr = audioClient->Start();
        if (SUCCEEDED(hr)) {
            printf("Playback started. Press Ctrl+C to stop.\n\n");
        } else {
            printf("AudioClient::Start failed: 0x%08X\n", hr);
            g_running = false;
        }
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // ---- Render loop ----
    uint64_t totalBytesPlayed = 0;
    uint64_t totalSilenceFrames = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (g_running) {
        DWORD waitRes = WaitForSingleObject(renderEvent, 200);
        if (waitRes == WAIT_TIMEOUT) continue;
        if (waitRes != WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        hr = audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 framesNeeded = bufferFrames - padding;
        if (framesNeeded == 0) continue;

        BYTE* wasapiBuf = NULL;
        hr = renderClient->GetBuffer(framesNeeded, &wasapiBuf);
        if (FAILED(hr)) {
            printf("GetBuffer failed: 0x%08X\n", hr);
            break;
        }
        size_t bytesNeeded = framesNeeded * frameSize;
        size_t bytesGot = ring.Read(wasapiBuf, bytesNeeded);
        if (bytesGot < bytesNeeded) {
            // Underrun: fill rest with silence
            memset(wasapiBuf + bytesGot, 0, bytesNeeded - bytesGot);
            totalSilenceFrames += (bytesNeeded - bytesGot) / frameSize;
        }
        renderClient->ReleaseBuffer(framesNeeded, 0);
        totalBytesPlayed += bytesGot;
    }

    audioClient->Stop();

    // Close socket first to wake up blocked recv() in network thread
    closesocket(dataSock);

    // Cleanup network thread
    if (recvThread) {
        WaitForSingleObject(recvThread, 1000);
        CloseHandle(recvThread);
    }

    // ---- Stats ----
    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    if (elapsed > 0) {
        printf("\n--- Receiver Stats ---\n");
        printf("Duration:      %lld ms\n", elapsed);
        printf("Bytes played:  %llu\n", totalBytesPlayed);
        printf("Silence frames:%llu (%.1f%%)\n", totalSilenceFrames,
               totalSilenceFrames * 100.0 / (totalBytesPlayed / frameSize + totalSilenceFrames));
        printf("Avg bitrate:   %.2f Kbps\n", totalBytesPlayed * 8.0 / elapsed);
    }

    // ---- Cleanup ----
    WSACleanup();
    renderClient->Release();
    CloseHandle(renderEvent);
    CoTaskMemFree(mixFormat);
    audioClient->Release();
    renderDevice->Release();
    enumerator->Release();
    CoUninitialize();
    printf("Receiver stopped.\n");
    return 0;
}

// ======================================================================
// main
// ======================================================================
int main(int argc, char* argv[]) {
    Config cfg;
    if (!ParseArgs(argc, argv, cfg)) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (cfg.list_devices) {
        ListAudioDevices();
        return 0;
    }

    if (cfg.mode == "sender") {
        return RunSender(cfg);
    } else {
        return RunReceiver(cfg);
    }
}
