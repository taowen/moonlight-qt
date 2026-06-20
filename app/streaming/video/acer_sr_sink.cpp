#include "acer_sr_sink.h"

#ifdef _WIN32

#include "SDL_compat.h"

#include <libavutil/hwcontext_d3d11va.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace {

constexpr size_t kMaxQueuedPackets = 3;

bool resolveDllPath(wchar_t* path, size_t count)
{
    DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(count));
    if (!len || len >= count) {
        return false;
    }

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return false;
    }

    const wchar_t* dllName = L"moonlight-acer-sr.dll";
    size_t prefix = static_cast<size_t>(slash - path) + 1;
    size_t remaining = count - prefix;
    if (wcslen(dllName) + 1 > remaining) {
        return false;
    }

    wcscpy_s(slash + 1, remaining, dllName);
    return true;
}

} // namespace

AcerSrVideoSink::AcerSrVideoSink() = default;

AcerSrVideoSink::~AcerSrVideoSink()
{
    close();
}

bool AcerSrVideoSink::isEnabledByEnvironment()
{
    char value[16] = {};
    DWORD len = GetEnvironmentVariableA("MOONLIGHT_ACER_SR", value,
                                        static_cast<DWORD>(sizeof(value)));
    if (!len || len >= sizeof(value)) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
            _stricmp(value, "true") == 0 ||
            _stricmp(value, "yes") == 0 ||
            _stricmp(value, "on") == 0;
}

enum AVPixelFormat
AcerSrVideoSink::getHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts)
{
    (void) ctx;

    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Acer SR: D3D11VA hardware pixel format is unavailable");
    return AV_PIX_FMT_NONE;
}

bool AcerSrVideoSink::loadDll()
{
    wchar_t dllPath[MAX_PATH];
    if (!resolveDllPath(dllPath, std::size(dllPath))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not resolve moonlight-acer-sr.dll path");
        return false;
    }

    m_Dll = LoadLibraryW(dllPath);
    if (!m_Dll) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not load moonlight-acer-sr.dll (error %lu)",
                     GetLastError());
        return false;
    }

#define LOAD_PROC(NAME, MEMBER) \
    MEMBER = reinterpret_cast<decltype(MEMBER)>(GetProcAddress(m_Dll, "moonlight_acer_sr_" NAME)); \
    if (!MEMBER) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, \
                     "Acer SR: missing DLL export moonlight_acer_sr_%s", NAME); \
        unloadDll(); \
        return false; \
    }

    LOAD_PROC("init_d3d11", m_InitD3d11);
    LOAD_PROC("push_d3d11", m_PushD3d11);
    LOAD_PROC("shutdown", m_Shutdown);
    LOAD_PROC("last_error", m_LastError);

#undef LOAD_PROC

    return true;
}

void AcerSrVideoSink::unloadDll()
{
    if (m_Dll) {
        FreeLibrary(m_Dll);
        m_Dll = nullptr;
    }
    m_InitD3d11 = nullptr;
    m_PushD3d11 = nullptr;
    m_Shutdown = nullptr;
    m_LastError = nullptr;
}

const char* AcerSrVideoSink::lastDllError() const
{
    return m_LastError ? m_LastError() : "";
}

bool AcerSrVideoSink::open(const AVCodecContext* sourceCtx)
{
    if (m_Opened) {
        return true;
    }
    if (!sourceCtx || sourceCtx->codec_type != AVMEDIA_TYPE_VIDEO) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: expected a video decoder context");
        return false;
    }
    if (sourceCtx->width <= 0 || sourceCtx->height <= 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: invalid source size %dx%d",
                     sourceCtx->width, sourceCtx->height);
        return false;
    }

    if (!loadDll()) {
        return false;
    }

    int ret = av_hwdevice_ctx_create(&m_HwDeviceCtx,
                                     AV_HWDEVICE_TYPE_D3D11VA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not create D3D11VA device: %d", ret);
        goto fail;
    }

    {
        AVHWDeviceContext* hwctx =
            reinterpret_cast<AVHWDeviceContext*>(m_HwDeviceCtx->data);
        auto* d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
        d3d11->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

        if (!m_InitD3d11(d3d11->device, sourceCtx->width, sourceCtx->height)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: %s", lastDllError());
            goto fail;
        }
    }

    if (!openDecoder(sourceCtx)) {
        goto fail_shutdown;
    }

    m_Frame = av_frame_alloc();
    if (!m_Frame) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Acer SR: frame allocation failed");
        goto fail_decoder;
    }

    m_StopWorker = false;
    m_Failed = false;
    m_Worker = std::thread(&AcerSrVideoSink::workerLoop, this);
    m_Opened = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Acer SR sidecar enabled for %dx%d source video",
                sourceCtx->width, sourceCtx->height);
    return true;

fail_decoder:
    avcodec_free_context(&m_CodecCtx);
fail_shutdown:
    if (m_Shutdown) {
        m_Shutdown();
    }
fail:
    av_buffer_unref(&m_HwDeviceCtx);
    unloadDll();
    return false;
}

bool AcerSrVideoSink::openDecoder(const AVCodecContext* sourceCtx)
{
    const AVCodec* codec = avcodec_find_decoder(sourceCtx->codec_id);
    if (!codec) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not find decoder for codec %d",
                     sourceCtx->codec_id);
        return false;
    }

    m_CodecCtx = avcodec_alloc_context3(codec);
    if (!m_CodecCtx) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: decoder context allocation failed");
        return false;
    }

    m_CodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_CodecCtx->width = sourceCtx->width;
    m_CodecCtx->height = sourceCtx->height;
    m_CodecCtx->pkt_timebase = sourceCtx->pkt_timebase;
    m_CodecCtx->pix_fmt = AV_PIX_FMT_D3D11;
    m_CodecCtx->get_format = AcerSrVideoSink::getHwFormat;
    m_CodecCtx->thread_count = 1;
    m_CodecCtx->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    if (!m_CodecCtx->hw_device_ctx) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: hardware device ref allocation failed");
        avcodec_free_context(&m_CodecCtx);
        return false;
    }

    if (sourceCtx->extradata && sourceCtx->extradata_size > 0) {
        m_CodecCtx->extradata =
            static_cast<uint8_t*>(av_mallocz(sourceCtx->extradata_size +
                                             AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_CodecCtx->extradata) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: decoder extradata allocation failed");
            avcodec_free_context(&m_CodecCtx);
            return false;
        }
        memcpy(m_CodecCtx->extradata, sourceCtx->extradata,
               sourceCtx->extradata_size);
        m_CodecCtx->extradata_size = sourceCtx->extradata_size;
    }

    if (avcodec_open2(m_CodecCtx, codec, nullptr) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not open D3D11VA decoder");
        avcodec_free_context(&m_CodecCtx);
        return false;
    }

    return true;
}

void AcerSrVideoSink::close()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_StopWorker = true;
        m_Queue.clear();
    }
    m_Cv.notify_all();
    if (m_Worker.joinable()) {
        m_Worker.join();
    }

    if (m_Frame) {
        av_frame_free(&m_Frame);
    }
    if (m_CodecCtx) {
        avcodec_free_context(&m_CodecCtx);
    }
    if (m_Shutdown) {
        m_Shutdown();
    }
    av_buffer_unref(&m_HwDeviceCtx);
    unloadDll();
    m_Opened = false;
    m_LoggedFirstFrame = false;
    m_Failed = false;
}

bool AcerSrVideoSink::pushPacket(const AVPacket* packet)
{
    if (!m_Opened || m_Failed || !packet || !packet->data || packet->size <= 0) {
        return false;
    }

    QueuedPacket queued;
    queued.data.assign(packet->data, packet->data + packet->size);
    queued.flags = packet->flags;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        while (m_Queue.size() >= kMaxQueuedPackets) {
            m_Queue.pop_front();
        }
        m_Queue.emplace_back(std::move(queued));
    }
    m_Cv.notify_one();
    return true;
}

void AcerSrVideoSink::workerLoop()
{
    for (;;) {
        QueuedPacket queued;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait(lock, [this] {
                return m_StopWorker || !m_Queue.empty();
            });
            if (m_StopWorker && m_Queue.empty()) {
                return;
            }
            queued = std::move(m_Queue.front());
            m_Queue.pop_front();
        }

        if (!decodePacket(queued)) {
            m_Failed = true;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: disabling sidecar after decode/render failure");
            return;
        }
    }
}

bool AcerSrVideoSink::decodePacket(const QueuedPacket& queued)
{
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    if (av_new_packet(packet, static_cast<int>(queued.data.size())) < 0) {
        av_packet_free(&packet);
        return false;
    }
    memcpy(packet->data, queued.data.data(), queued.data.size());
    packet->flags = queued.flags;

    int ret = avcodec_send_packet(m_CodecCtx, packet);
    av_packet_free(&packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Acer SR: could not send packet to D3D11VA decoder: %d",
                     ret);
        return false;
    }

    return receiveFrames();
}

bool AcerSrVideoSink::receiveFrames()
{
    for (;;) {
        int ret = avcodec_receive_frame(m_CodecCtx, m_Frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: could not receive D3D11VA frame: %d", ret);
            return false;
        }

        if (m_Frame->format != AV_PIX_FMT_D3D11) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: expected AV_PIX_FMT_D3D11, got %d",
                         m_Frame->format);
            av_frame_unref(m_Frame);
            return false;
        }

        if (!m_LoggedFirstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Acer SR: first D3D11VA texture frame received");
            m_LoggedFirstFrame = true;
        }

        void* texture = m_Frame->data[0];
        unsigned subresource = static_cast<unsigned>(
            reinterpret_cast<uintptr_t>(m_Frame->data[1]));
        if (!m_PushD3d11(texture, subresource)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Acer SR: %s", lastDllError());
            av_frame_unref(m_Frame);
            return false;
        }

        av_frame_unref(m_Frame);
    }
}

#endif
