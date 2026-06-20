#pragma once

#ifdef _WIN32

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

class AcerSrVideoSink {
public:
    AcerSrVideoSink();
    ~AcerSrVideoSink();

    static bool isEnabledByEnvironment();

    bool open(const AVCodecContext* sourceCtx);
    void close();
    bool pushPacket(const AVPacket* packet);

private:
    struct QueuedPacket {
        std::vector<uint8_t> data;
        int flags = 0;
    };

    static enum AVPixelFormat getHwFormat(AVCodecContext* ctx,
                                          const enum AVPixelFormat* pixFmts);

    bool loadDll();
    void unloadDll();
    bool openDecoder(const AVCodecContext* sourceCtx);
    bool initD3d11Output(int sourceWidth, int sourceHeight);
    void workerLoop();
    bool decodePacket(const QueuedPacket& queued);
    bool receiveFrames();
    const char* lastDllError() const;

    HMODULE m_Dll = nullptr;
    int (*m_InitD3d11)(void* device, int sourceWidth, int sourceHeight) = nullptr;
    int (*m_PushD3d11)(void* texture, unsigned subresource) = nullptr;
    void (*m_Shutdown)() = nullptr;
    const char* (*m_LastError)() = nullptr;

    AVBufferRef* m_HwDeviceCtx = nullptr;
    AVCodecContext* m_CodecCtx = nullptr;
    AVFrame* m_Frame = nullptr;

    std::thread m_Worker;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<QueuedPacket> m_Queue;
    bool m_StopWorker = false;
    bool m_Opened = false;
    bool m_LoggedFirstFrame = false;
    std::atomic<bool> m_Failed{false};
};

#endif
