#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#define SAFE_RELEASE(P) do { if (P) { (P)->Release(); (P) = nullptr; } } while (0)

namespace {

using InitD3d11Fn = int (*)(void*, int, int);
using PushD3d11Fn = int (*)(void*, unsigned);
using UpdateDepthU16Fn = int (*)(const void*, int, int);
using ShutdownFn = void (*)();
using LastErrorFn = const char* (*)();

constexpr int kDepthSize = 518;

struct Options {
    int width = 1920;
    int height = 1080;
    int frames = 600;
    int fps = 60;
    bool synthetic_depth = true;
};

struct Sidecar {
    HMODULE dll = nullptr;
    InitD3d11Fn init = nullptr;
    PushD3d11Fn push = nullptr;
    UpdateDepthU16Fn update_depth = nullptr;
    ShutdownFn shutdown = nullptr;
    LastErrorFn last_error = nullptr;
};

struct Rgb {
    float r;
    float g;
    float b;
};

void print_usage() {
    std::printf(
        "Usage: moonlight-acer-sr-smoke [--width N] [--height N] "
        "[--frames N] [--fps N] [--no-depth]\n"
    );
}

bool parse_int_arg(const wchar_t* value, int& out) {
    if (!value || !*value) {
        return false;
    }
    wchar_t* end = nullptr;
    long parsed = std::wcstol(value, &end, 10);
    if (!end || *end != L'\0' || parsed <= 0 || parsed > 32768) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_options(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        auto next_int = [&](int& target) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            return parse_int_arg(argv[++i], target);
        };

        if (std::wcscmp(arg, L"--width") == 0) {
            if (!next_int(options.width)) {
                return false;
            }
        } else if (std::wcscmp(arg, L"--height") == 0) {
            if (!next_int(options.height)) {
                return false;
            }
        } else if (std::wcscmp(arg, L"--frames") == 0) {
            if (!next_int(options.frames)) {
                return false;
            }
        } else if (std::wcscmp(arg, L"--fps") == 0) {
            if (!next_int(options.fps)) {
                return false;
            }
        } else if (std::wcscmp(arg, L"--no-depth") == 0) {
            options.synthetic_depth = false;
        } else if (std::wcscmp(arg, L"--help") == 0 ||
                   std::wcscmp(arg, L"-h") == 0) {
            print_usage();
            std::exit(0);
        } else {
            return false;
        }
    }

    if ((options.width % 2) != 0 || (options.height % 2) != 0) {
        std::printf("NV12 test frames require even width and height.\n");
        return false;
    }
    options.fps = std::max(1, std::min(options.fps, 240));
    options.frames = std::max(1, options.frames);
    return true;
}

bool resolve_sidecar_path(wchar_t* path, size_t count) {
    DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(count));
    if (!len || len >= count) {
        return false;
    }

    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash) {
        return false;
    }

    const wchar_t* dll_name = L"moonlight-acer-sr.dll";
    size_t prefix = static_cast<size_t>(slash - path) + 1;
    if (std::wcslen(dll_name) + 1 > count - prefix) {
        return false;
    }
    wcscpy_s(slash + 1, count - prefix, dll_name);
    return true;
}

template <typename T>
bool load_proc(HMODULE dll, const char* name, T& proc) {
    proc = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!proc) {
        std::printf("Missing DLL export: %s\n", name);
        return false;
    }
    return true;
}

bool load_sidecar(Sidecar& sidecar) {
    wchar_t path[MAX_PATH] = {};
    if (!resolve_sidecar_path(path, std::size(path))) {
        std::printf("Could not resolve moonlight-acer-sr.dll path.\n");
        return false;
    }

    sidecar.dll = LoadLibraryW(path);
    if (!sidecar.dll) {
        std::printf("LoadLibraryW(%ls) failed: %lu\n", path, GetLastError());
        return false;
    }

    return load_proc(sidecar.dll, "moonlight_acer_sr_init_d3d11", sidecar.init) &&
           load_proc(sidecar.dll, "moonlight_acer_sr_push_d3d11", sidecar.push) &&
           load_proc(sidecar.dll, "moonlight_acer_sr_update_depth_u16", sidecar.update_depth) &&
           load_proc(sidecar.dll, "moonlight_acer_sr_shutdown", sidecar.shutdown) &&
           load_proc(sidecar.dll, "moonlight_acer_sr_last_error", sidecar.last_error);
}

uint8_t clamp_byte(float value) {
    int rounded = static_cast<int>(std::lround(value));
    return static_cast<uint8_t>(std::max(0, std::min(255, rounded)));
}

Rgb pixel_rgb(int x, int y, int width, int height, int frame) {
    float fx = static_cast<float>(x) / static_cast<float>(std::max(width - 1, 1));
    float fy = static_cast<float>(y) / static_cast<float>(std::max(height - 1, 1));
    float t = static_cast<float>(frame) * 0.018f;

    Rgb color {
        0.08f + 0.18f * fx,
        0.10f + 0.26f * fy,
        0.16f + 0.22f * (1.0f - fx),
    };

    int bar = (x / std::max(width / 12, 1)) % 3;
    if (bar == 0) {
        color.r += 0.12f;
    } else if (bar == 1) {
        color.g += 0.12f;
    } else {
        color.b += 0.12f;
    }

    float cx = 0.50f + 0.22f * std::sin(t);
    float cy = 0.48f + 0.12f * std::cos(t * 0.73f);
    float dx = (fx - cx) / 0.18f;
    float dy = (fy - cy) / 0.28f;
    if (dx * dx + dy * dy < 1.0f) {
        color = {0.95f, 0.73f, 0.32f};
    }

    if (fx > 0.08f && fx < 0.30f && fy > 0.56f && fy < 0.82f) {
        color = {0.25f, 0.76f, 0.93f};
    }

    float grid = (x % std::max(width / 16, 1) == 0 ||
                  y % std::max(height / 10, 1) == 0) ? 0.10f : 0.0f;
    color.r = std::min(1.0f, color.r + grid);
    color.g = std::min(1.0f, color.g + grid);
    color.b = std::min(1.0f, color.b + grid);
    return color;
}

void rgb_to_yuv(const Rgb& rgb, uint8_t& y, uint8_t& u, uint8_t& v) {
    y = clamp_byte(16.0f + 219.0f *
                   (0.299f * rgb.r + 0.587f * rgb.g + 0.114f * rgb.b));
    u = clamp_byte(128.0f + 224.0f *
                   (-0.168736f * rgb.r - 0.331264f * rgb.g + 0.5f * rgb.b));
    v = clamp_byte(128.0f + 224.0f *
                   (0.5f * rgb.r - 0.418688f * rgb.g - 0.081312f * rgb.b));
}

void fill_nv12_frame(std::vector<uint8_t>& frame_data,
                     int width,
                     int height,
                     int frame) {
    uint8_t* y_plane = frame_data.data();
    uint8_t* uv_plane = y_plane + width * height;

    for (int y = 0; y < height; ++y) {
        uint8_t* row = y_plane + y * width;
        for (int x = 0; x < width; ++x) {
            uint8_t yy = 0;
            uint8_t uu = 0;
            uint8_t vv = 0;
            rgb_to_yuv(pixel_rgb(x, y, width, height, frame), yy, uu, vv);
            row[x] = yy;
        }
    }

    for (int y = 0; y < height; y += 2) {
        uint8_t* row = uv_plane + (y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            float u_sum = 0.0f;
            float v_sum = 0.0f;
            for (int yy = 0; yy < 2; ++yy) {
                for (int xx = 0; xx < 2; ++xx) {
                    uint8_t yv = 0;
                    uint8_t uv = 0;
                    uint8_t vv = 0;
                    rgb_to_yuv(pixel_rgb(x + xx, y + yy, width, height, frame),
                               yv, uv, vv);
                    u_sum += uv;
                    v_sum += vv;
                }
            }
            row[x] = clamp_byte(u_sum * 0.25f);
            row[x + 1] = clamp_byte(v_sum * 0.25f);
        }
    }
}

void fill_depth_frame(std::vector<uint16_t>& depth, int frame) {
    float t = static_cast<float>(frame) * 0.018f;
    float cx = 0.50f + 0.22f * std::sin(t);
    float cy = 0.48f + 0.12f * std::cos(t * 0.73f);

    for (int y = 0; y < kDepthSize; ++y) {
        float fy = (static_cast<float>(y) + 0.5f) / kDepthSize;
        for (int x = 0; x < kDepthSize; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) / kDepthSize;
            float d = 0.18f + 0.22f * (1.0f - fy);

            float ex = (fx - cx) / 0.18f;
            float ey = (fy - cy) / 0.28f;
            if (ex * ex + ey * ey < 1.0f) {
                d = 0.88f;
            }

            if (fx > 0.08f && fx < 0.30f && fy > 0.56f && fy < 0.82f) {
                d = std::max(d, 0.58f);
            }

            int idx = y * kDepthSize + x;
            depth[idx] = static_cast<uint16_t>(std::max(
                0,
                std::min(65535, static_cast<int>(std::lround(d * 65535.0f)))
            ));
        }
    }
}

bool create_d3d11_device(ID3D11Device** device, ID3D11DeviceContext** context) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        device,
        &created,
        context
    );
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            &levels[1],
            1,
            D3D11_SDK_VERSION,
            device,
            &created,
            context
        );
    }

    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed: 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    UINT support = 0;
    hr = (*device)->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
    if (FAILED(hr) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        std::printf("DXGI_FORMAT_NV12 is not supported by this D3D11 device.\n");
        return false;
    }
    return true;
}

bool create_nv12_texture(ID3D11Device* device,
                         int width,
                         int height,
                         ID3D11Texture2D** texture) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, texture);
    if (FAILED(hr)) {
        std::printf("CreateTexture2D(NV12) failed: 0x%08X\n",
                    static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    Sidecar sidecar;
    if (!load_sidecar(sidecar)) {
        return 1;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* texture = nullptr;

    if (!create_d3d11_device(&device, &context) ||
        !create_nv12_texture(device, options.width, options.height, &texture)) {
        SAFE_RELEASE(texture);
        SAFE_RELEASE(context);
        SAFE_RELEASE(device);
        if (sidecar.dll) {
            FreeLibrary(sidecar.dll);
        }
        return 1;
    }

    if (!sidecar.init(device, options.width, options.height)) {
        std::printf("moonlight_acer_sr_init_d3d11 failed: %s\n",
                    sidecar.last_error ? sidecar.last_error() : "");
        SAFE_RELEASE(texture);
        SAFE_RELEASE(context);
        SAFE_RELEASE(device);
        FreeLibrary(sidecar.dll);
        return 1;
    }

    std::vector<uint8_t> nv12(
        static_cast<size_t>(options.width) * options.height * 3 / 2
    );
    std::vector<uint16_t> depth(kDepthSize * kDepthSize);

    using Clock = std::chrono::steady_clock;
    const auto frame_interval =
        std::chrono::duration<double>(1.0 / static_cast<double>(options.fps));
    double total_push_ms = 0.0;
    double max_push_ms = 0.0;
    int pushed = 0;

    std::printf("Acer SR smoke: %dx%d, frames=%d, fps=%d, syntheticDepth=%s\n",
                options.width,
                options.height,
                options.frames,
                options.fps,
                options.synthetic_depth ? "yes" : "no");

    auto next_frame_time = Clock::now();
    for (int frame = 0; frame < options.frames; ++frame) {
        fill_nv12_frame(nv12, options.width, options.height, frame);
        context->UpdateSubresource(
            texture,
            0,
            nullptr,
            nv12.data(),
            static_cast<UINT>(options.width),
            static_cast<UINT>(nv12.size())
        );

        if (options.synthetic_depth) {
            fill_depth_frame(depth, frame);
            if (!sidecar.update_depth(depth.data(), kDepthSize, kDepthSize)) {
                std::printf("moonlight_acer_sr_update_depth_u16 failed: %s\n",
                            sidecar.last_error ? sidecar.last_error() : "");
                break;
            }
        }

        auto push_start = Clock::now();
        if (!sidecar.push(texture, 0)) {
            std::printf("moonlight_acer_sr_push_d3d11 failed: %s\n",
                        sidecar.last_error ? sidecar.last_error() : "");
            break;
        }
        auto push_end = Clock::now();

        double push_ms =
            std::chrono::duration<double, std::milli>(push_end - push_start).count();
        total_push_ms += push_ms;
        max_push_ms = std::max(max_push_ms, push_ms);
        ++pushed;

        if (pushed == 1 || pushed % std::max(options.fps, 1) == 0) {
            std::printf("frame=%d pushMs=%.3f avgPushMs=%.3f\n",
                        pushed,
                        push_ms,
                        total_push_ms / pushed);
        }

        next_frame_time += std::chrono::duration_cast<Clock::duration>(frame_interval);
        std::this_thread::sleep_until(next_frame_time);
    }

    std::printf("Acer SR smoke done: pushed=%d avgPushMs=%.3f maxPushMs=%.3f\n",
                pushed,
                pushed ? total_push_ms / pushed : 0.0,
                max_push_ms);

    sidecar.shutdown();
    SAFE_RELEASE(texture);
    SAFE_RELEASE(context);
    SAFE_RELEASE(device);
    FreeLibrary(sidecar.dll);
    return pushed > 0 ? 0 : 1;
}
