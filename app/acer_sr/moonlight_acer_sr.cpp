#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#define SRDISPLAY_LAZYBINDING

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <onnxruntime_c_api.h>

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/utility/exception.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#define SAFE_RELEASE(P) do { if (P) { (P)->Release(); (P) = nullptr; } } while (0)

namespace {

std::string g_last_error;
constexpr int kRequiredDepthWidth = 588;
constexpr int kRequiredDepthHeight = 336;

const char *kFullscreenVs = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0),
    };

    VSOut outv;
    outv.pos = float4(pos[id], 0.0, 1.0);
    outv.uv = float2((pos[id].x + 1.0) * 0.5,
                     (1.0 - pos[id].y) * 0.5);
    return outv;
}
)";

const char *kNv12ToRgbPs = R"(
Texture2D y_tex : register(t0);
Texture2D uv_tex : register(t1);
Texture2D depth_tex : register(t2);
SamplerState linear_sampler : register(s0);

cbuffer ConvertConstants : register(b0) {
    float4 dims;       // sourceW, sourceH, perEyeW, perEyeH
    float4 stereo;     // hasDepth, maxShiftPx, parallaxBalance, convergence
    float4 depthDims;  // depthW, depthH, renderMode, stereoStrength
    float4 shifts;     // fgShiftPx, mgShiftPx, bgShiftPx, fgPopMultiplier
    float4 tuning0;    // bgPushMultiplier, depthMid, subjectLockStrength, convergencePx
    float4 tuning1;    // edgeLowPx, edgeHighPx, edgeShiftScale, repairStrength
    float4 tuning2;    // repairLowPx, repairHighPx, lumaProtectStrength, unused
};

static const float RENDER_MODE_FLAT = 0.0;
static const float RENDER_MODE_STEREO = 1.0;
static const float RENDER_MODE_DEPTH_MAP = 2.0;
static const float STEREO_STRENGTH_WEAK = 0.0;

float4 sample_source(float2 uv) {
    float y = y_tex.Sample(linear_sampler, uv).r;
    float2 cbcr = uv_tex.Sample(linear_sampler, uv).rg;

    float yy = 1.16438356 * (y - 0.0625);
    float cb = cbcr.x - 0.5;
    float cr = cbcr.y - 0.5;

    float3 rgb;
    rgb.r = yy + 1.79274107 * cr;
    rgb.g = yy - 0.21324861 * cb - 0.53290933 * cr;
    rgb.b = yy + 2.11240179 * cb;
    return float4(saturate(rgb), 1.0);
}

float reflect01(float value) {
    if (value < 0.0) {
        value = -value;
    }
    if (value > 1.0) {
        value = 2.0 - value;
    }
    return saturate(value);
}

float2 reflect_uv(float2 uv) {
    return float2(reflect01(uv.x), saturate(uv.y));
}

float depth_sample(float2 uv) {
    float d = depth_tex.Sample(linear_sampler, saturate(uv)).r;
    return pow(saturate(d), 1.2);
}

float foreground_scale_depth(float d) {
    const float scale = 0.05;
    const float mid = 0.5;
    float dist = d - mid;
    float exponent = 1.0 / (1.0 + scale);
    return saturate(mid + sign(dist) * pow(abs(dist), exponent));
}

float smoothed_depth_sample(float2 uv) {
    return foreground_scale_depth(depth_sample(uv));
}

float smoothed_depth(float2 uv) {
    float2 texel = 1.0 / max(depthDims.xy, float2(1.0, 1.0));
    float d = smoothed_depth_sample(uv) * 4.0;
    d += smoothed_depth_sample(uv + float2(texel.x, 0.0)) * 2.0;
    d += smoothed_depth_sample(uv - float2(texel.x, 0.0)) * 2.0;
    d += smoothed_depth_sample(uv + float2(0.0, texel.y)) * 2.0;
    d += smoothed_depth_sample(uv - float2(0.0, texel.y)) * 2.0;
    d += smoothed_depth_sample(uv + texel) * 1.0;
    d += smoothed_depth_sample(uv - texel) * 1.0;
    d += smoothed_depth_sample(uv + float2(texel.x, -texel.y)) * 1.0;
    d += smoothed_depth_sample(uv + float2(-texel.x, texel.y)) * 1.0;
    return d / 16.0;
}

float desktop_shift_px(float d) {
    const float ipdUv = 0.064;
    const float depthRatio = 2.0;
    const float depthStrength = 0.05;
    float shiftPx = (stereo.w - d) * ipdUv * depthRatio * depthStrength * dims.z;
    return clamp(shiftPx, -stereo.y, stereo.y);
}

float gaussian_weight(float d, float center, float sigma) {
    float x = (d - center) / sigma;
    return exp(-0.5 * x * x);
}

float shift_px_for_depth(float d) {
    float wf = gaussian_weight(d, 0.15, 0.24);
    float wm = gaussian_weight(d, tuning0.y, 0.28);
    float wb = gaussian_weight(d, 0.85, 0.24);
    float sumW = max(wf + wm + wb, 0.000001);
    wf /= sumW;
    wm /= sumW;
    wb /= sumW;

    float shiftPx =
        wf * shifts.x * shifts.w +
        wm * shifts.y +
        wb * shifts.z * tuning0.x;
    return shiftPx * stereo.z;
}

float base_shift_px(float2 uv) {
    return shift_px_for_depth(depth_sample(uv));
}

float smoothed_shift_px(float2 uv) {
    return shift_px_for_depth(smoothed_depth(uv));
}

float luma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

float signed_shift_gx(float2 uv, float2 depthTexel) {
    return base_shift_px(uv + float2(depthTexel.x, 0.0)) -
           base_shift_px(uv - float2(depthTexel.x, 0.0));
}

float trailing_mask_for_gx(float gx, float eyeSign) {
    float trailing = eyeSign > 0.0 ? max(gx, 0.0) : max(-gx, 0.0);
    return smoothstep(tuning2.x, tuning2.y, trailing);
}

float leading_mask_for_gx(float gx, float eyeSign) {
    float leading = eyeSign > 0.0 ? max(-gx, 0.0) : max(gx, 0.0);
    return smoothstep(tuning2.x, tuning2.y, leading);
}

float dilated_leading_mask(float2 uv, float2 depthTexel, float eyeSign) {
    float mask = leading_mask_for_gx(signed_shift_gx(uv, depthTexel), eyeSign);
    mask = max(mask, leading_mask_for_gx(signed_shift_gx(uv + float2(depthTexel.x, 0.0), depthTexel), eyeSign));
    mask = max(mask, leading_mask_for_gx(signed_shift_gx(uv - float2(depthTexel.x, 0.0), depthTexel), eyeSign));
    mask = max(mask, leading_mask_for_gx(signed_shift_gx(uv + float2(0.0, depthTexel.y), depthTexel), eyeSign));
    mask = max(mask, leading_mask_for_gx(signed_shift_gx(uv - float2(0.0, depthTexel.y), depthTexel), eyeSign));
    return saturate(mask * 1.20);
}

float image_edge_barrier(float2 uv, float2 colorTexel) {
    float3 cx0 = sample_source(saturate(uv - float2(colorTexel.x, 0.0))).rgb;
    float3 cx1 = sample_source(saturate(uv + float2(colorTexel.x, 0.0))).rgb;
    float3 cy0 = sample_source(saturate(uv - float2(0.0, colorTexel.y))).rgb;
    float3 cy1 = sample_source(saturate(uv + float2(0.0, colorTexel.y))).rgb;
    float edge = max(abs(luma(cx1) - luma(cx0)), abs(luma(cy1) - luma(cy0)));
    return smoothstep(0.035, 0.135, edge) * tuning2.z;
}

float3 background_aware_fill(float2 warpedUv, float2 sourceUv, float fillDir, float centerDepth) {
    float3 fill = float3(0.0, 0.0, 0.0);
    float3 fallback = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float fallbackWeightSum = 0.0;
    float2 verticalTexel = float2(0.0, 1.0 / max(dims.w, 1.0));

    [unroll]
    for (int i = 1; i <= 8; ++i) {
        float distancePx = (float)i;
        float spaceWeight = 1.0 / distancePx;
        float2 sampleUv = saturate(warpedUv + float2(fillDir * distancePx / max(dims.z, 1.0), 0.0));
        float3 sampleColor = sample_source(sampleUv).rgb;

        float2 depthUv = saturate(sourceUv + float2(fillDir * distancePx / max(dims.z, 1.0), 0.0));
        float sampleDepth = depth_sample(depthUv);
        float backgroundWeight = smoothstep(0.010, 0.090, sampleDepth - centerDepth);

        fill += sampleColor * spaceWeight * backgroundWeight;
        weightSum += spaceWeight * backgroundWeight;
        fallback += sampleColor * spaceWeight;
        fallbackWeightSum += spaceWeight;
    }

    fallback /= max(fallbackWeightSum, 0.000001);
    if (weightSum < 0.0001) {
        return fallback;
    }

    fill /= weightSum;
    float3 vertical = sample_source(saturate(warpedUv + verticalTexel)).rgb;
    vertical += sample_source(saturate(warpedUv - verticalTexel)).rgb;
    vertical *= 0.5;
    return lerp(fill, vertical, 0.08);
}

float2 fitted_source_uv(float2 eyeUv, out bool inside) {
    float source_aspect = dims.x / max(dims.y, 1.0);
    float eye_aspect = dims.z / max(dims.w, 1.0);
    float2 sample_uv = eyeUv;

    if (source_aspect > eye_aspect) {
        float content_h = eye_aspect / source_aspect;
        sample_uv.y = (eyeUv.y - (1.0 - content_h) * 0.5) / content_h;
    } else {
        float content_w = source_aspect / eye_aspect;
        sample_uv.x = (eyeUv.x - (1.0 - content_w) * 0.5) / content_w;
    }

    inside = sample_uv.x >= 0.0 && sample_uv.x <= 1.0 &&
             sample_uv.y >= 0.0 && sample_uv.y <= 1.0;
    return sample_uv;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 eye_uv = float2(frac(uv.x * 2.0), uv.y);
    bool inside = false;
    float2 sample_uv = fitted_source_uv(eye_uv, inside);
    if (!inside) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    if (depthDims.z < RENDER_MODE_STEREO + 0.5 || stereo.x < 0.5) {
        return sample_source(sample_uv);
    }

    if (depthDims.z > RENDER_MODE_STEREO + 0.5) {
        float d = smoothed_depth(sample_uv);
        return float4(d, d, d, 1.0);
    }

    float eye_sign = uv.x < 0.5 ? 1.0 : -1.0;
    if (depthDims.w < STEREO_STRENGTH_WEAK + 0.5) {
        float d = smoothed_depth(sample_uv);
        float shift_px = desktop_shift_px(d);
        float2 shifted_uv = reflect_uv(sample_uv + float2(eye_sign * shift_px / max(dims.z, 1.0), 0.0));
        return sample_source(shifted_uv);
    }

    float2 depthTexel = 1.0 / max(depthDims.xy, float2(1.0, 1.0));
    float shift_px = smoothed_shift_px(sample_uv);
    float subject_shift_px = shift_px_for_depth(tuning0.y) * tuning0.z;
    shift_px -= subject_shift_px;
    shift_px -= tuning0.w;

    float shiftL = base_shift_px(sample_uv - float2(depthTexel.x, 0.0));
    float shiftR = base_shift_px(sample_uv + float2(depthTexel.x, 0.0));
    float shiftU = base_shift_px(sample_uv - float2(0.0, depthTexel.y));
    float shiftD = base_shift_px(sample_uv + float2(0.0, depthTexel.y));
    float grad = abs(shiftR - shiftL) + 0.5 * abs(shiftD - shiftU);
    float edgeMask = smoothstep(tuning1.x, tuning1.y, grad);
    shift_px *= lerp(1.0, tuning1.z, edgeMask);

    float leftFade = smoothstep(0.00, 0.04, sample_uv.x);
    float rightFade = smoothstep(0.00, 0.04, 1.0 - sample_uv.x);
    float topFade = smoothstep(0.00, 0.02, sample_uv.y);
    float bottomFade = smoothstep(0.00, 0.02, 1.0 - sample_uv.y);
    float edgeFade = leftFade * rightFade * topFade * bottomFade;
    shift_px *= lerp(0.65, 1.0, edgeFade);
    shift_px = clamp(shift_px, -stereo.y, stereo.y);

    float2 warped_uv = saturate(sample_uv + float2(eye_sign * shift_px / max(dims.z, 1.0), 0.0));
    float3 color = sample_source(warped_uv).rgb;
    float3 original = sample_source(sample_uv).rgb;
    float stressBlend = edgeMask * lerp(0.10, 0.20, saturate(stereo.z));
    color = lerp(color, original, stressBlend);

    if (tuning1.w > 0.001) {
        float gx = shiftR - shiftL;
        float gradGate = smoothstep(tuning1.x * 0.55, tuning1.y, grad);
        float repairMask = trailing_mask_for_gx(gx, eye_sign) * gradGate;
        float protectMask = dilated_leading_mask(sample_uv, depthTexel, eye_sign);
        float2 colorTexel = float2(1.0 / max(dims.z, 1.0), 1.0 / max(dims.w, 1.0));
        float localLumaBarrier = max(
            image_edge_barrier(sample_uv, colorTexel),
            image_edge_barrier(warped_uv, colorTexel) * 0.75
        );
        protectMask = max(protectMask, localLumaBarrier * gradGate);
        repairMask *= 1.0 - protectMask;

        float fillDir = eye_sign > 0.0 ? 1.0 : -1.0;
        float3 fill = background_aware_fill(warped_uv, sample_uv, fillDir, depth_sample(sample_uv));
        color = lerp(color, fill, repairMask * tuning1.w);
    }

    return float4(color, 1.0);
}
)";

struct ConvertConstants {
    float source_width;
    float source_height;
    float per_eye_width;
    float per_eye_height;
    float has_depth;
    float max_shift_px;
    float parallax_balance;
    float convergence;
    float depth_width;
    float depth_height;
    float render_mode;
    float stereo_strength;
    float fg_shift_px;
    float mg_shift_px;
    float bg_shift_px;
    float fg_pop_multiplier;
    float bg_push_multiplier;
    float depth_mid;
    float subject_lock_strength;
    float convergence_px;
    float edge_low_px;
    float edge_high_px;
    float edge_shift_scale;
    float repair_strength;
    float repair_low_px;
    float repair_high_px;
    float luma_protect_strength;
    float reserved0;
};

enum StereoRenderMode {
    kRenderModeFlat = 0,
    kRenderModeStereo = 1,
    kRenderModeDepthMap = 2,
};

enum StereoStrength {
    kStereoStrengthWeak = 0,
    kStereoStrengthMedium = 1,
    kStereoStrengthStrong = 2,
};

bool read_env_value(const char *name, char *value, size_t value_size) {
    if (!value || value_size == 0) {
        return false;
    }
    DWORD len = GetEnvironmentVariableA(name, value,
                                        static_cast<DWORD>(value_size));
    if (!len || len >= value_size) {
        value[0] = '\0';
        return false;
    }
    return true;
}

int read_render_mode_from_environment() {
    char value[32] = {};
    if (!read_env_value("MOONLIGHT_ACER_SR_MODE", value, sizeof(value))) {
        return kRenderModeStereo;
    }
    if (strcmp(value, "0") == 0 || _stricmp(value, "flat") == 0 ||
        _stricmp(value, "plane") == 0 || _stricmp(value, "2d") == 0) {
        return kRenderModeFlat;
    }
    if (strcmp(value, "2") == 0 || _stricmp(value, "depth") == 0 ||
        _stricmp(value, "depth_map") == 0) {
        return kRenderModeDepthMap;
    }
    return kRenderModeStereo;
}

int read_stereo_strength_from_environment() {
    char value[32] = {};
    if (!read_env_value("MOONLIGHT_ACER_SR_STRENGTH", value, sizeof(value))) {
        return kStereoStrengthWeak;
    }
    if (strcmp(value, "1") == 0 || _stricmp(value, "medium") == 0 ||
        _stricmp(value, "mid") == 0) {
        return kStereoStrengthMedium;
    }
    if (strcmp(value, "2") == 0 || _stricmp(value, "strong") == 0 ||
        _stricmp(value, "high") == 0) {
        return kStereoStrengthStrong;
    }
    return kStereoStrengthWeak;
}

const char *render_mode_name(int mode) {
    switch (mode) {
        case kRenderModeFlat:
            return "flat";
        case kRenderModeDepthMap:
            return "depth";
        case kRenderModeStereo:
        default:
            return "stereo";
    }
}

const char *stereo_strength_name(int strength) {
    switch (strength) {
        case kStereoStrengthMedium:
            return "medium";
        case kStereoStrengthStrong:
            return "strong";
        case kStereoStrengthWeak:
        default:
            return "weak";
    }
}

bool env_flag_enabled(const char* name) {
    char value[16] = {};
    DWORD len = GetEnvironmentVariableA(name, value,
                                        static_cast<DWORD>(sizeof(value)));
    if (!len || len >= sizeof(value)) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           _stricmp(value, "true") == 0 ||
           _stricmp(value, "yes") == 0 ||
           _stricmp(value, "on") == 0;
}

void set_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_error = buf;
    OutputDebugStringA(("moonlight-acer-sr: " + g_last_error + "\n").c_str());
    if (env_flag_enabled("MOONLIGHT_ACER_SR_CONSOLE_LOG")) {
        fprintf(stderr, "moonlight-acer-sr: %s\n", g_last_error.c_str());
        fflush(stderr);
    }
}

void log_message(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(("moonlight-acer-sr: " + std::string(buf) + "\n").c_str());
    if (env_flag_enabled("MOONLIGHT_ACER_SR_CONSOLE_LOG")) {
        fprintf(stderr, "moonlight-acer-sr: %s\n", buf);
        fflush(stderr);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

bool fill_rect_from_sr_location(SR_recti loc, RECT& out) {
    int64_t w = loc.right - loc.left;
    int64_t h = loc.bottom - loc.top;
    if (w <= 0 || h <= 0) {
        return false;
    }

    out.left = static_cast<LONG>(loc.left);
    out.top = static_cast<LONG>(loc.top);
    out.right = static_cast<LONG>(loc.right);
    out.bottom = static_cast<LONG>(loc.bottom);
    return true;
}

class DepthEstimator {
public:
    ~DepthEstimator() {
        shutdown();
    }

    bool initialize_from_environment() {
        wchar_t path[32768] = {};
        DWORD len = GetEnvironmentVariableW(L"MOONLIGHT_ACER_SR_DEPTH_MODEL",
                                            path,
                                            static_cast<DWORD>(std::size(path)));
        if (!len || len >= std::size(path)) {
            log_message("Depth model is required: set MOONLIGHT_ACER_SR_DEPTH_MODEL");
            return false;
        }

        model_path_ = path;
        ort_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (!ort_) {
            log_message("Depth model disabled: OrtGetApi failed");
            return false;
        }

        if (!ok(ort_->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                "moonlight-acer-sr-depth",
                                &env_),
                "CreateEnv")) {
            return false;
        }
        if (!ok(ort_->CreateSessionOptions(&session_options_),
                "CreateSessionOptions")) {
            return false;
        }
        ok(ort_->SetIntraOpNumThreads(session_options_, 1),
           "SetIntraOpNumThreads");
        ok(ort_->SetInterOpNumThreads(session_options_, 1),
           "SetInterOpNumThreads");
        ok(ort_->SetSessionGraphOptimizationLevel(session_options_,
                                                  ORT_ENABLE_ALL),
           "SetSessionGraphOptimizationLevel");

        std::vector<const char*> keys;
        std::vector<const char*> values;
        keys.push_back("device_type");
        values.push_back("NPU");
        keys.push_back("enable_qdq_optimizer");
        values.push_back("True");
        keys.push_back("load_config");
        values.push_back("{\"NPU\":{\"PERFORMANCE_HINT\":\"LATENCY\",\"EXECUTION_MODE_HINT\":\"PERFORMANCE\"}}");
        log_message("Depth model using required OpenVINO NPU QDQ latency path");

        if (!ok(ort_->SessionOptionsAppendExecutionProvider_OpenVINO_V2(
                    session_options_,
                    keys.data(),
                    values.data(),
                    keys.size()),
                "SessionOptionsAppendExecutionProvider_OpenVINO_V2")) {
            return false;
        }

        auto start = std::chrono::steady_clock::now();
        if (!ok(ort_->CreateSession(env_, model_path_.c_str(),
                                    session_options_, &session_),
                "CreateSession")) {
            return false;
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        log_message("Depth model loaded with OpenVINO NPU in %.1f ms", ms);

        if (!ok(ort_->GetAllocatorWithDefaultOptions(&allocator_),
                "GetAllocatorWithDefaultOptions")) {
            return false;
        }
        if (!ok(ort_->SessionGetInputName(session_, 0, allocator_, &input_name_),
                "SessionGetInputName")) {
            return false;
        }
        if (!ok(ort_->SessionGetOutputName(session_, 0, allocator_, &output_name_),
                "SessionGetOutputName")) {
            return false;
        }
        if (!validate_model_io()) {
            return false;
        }
        if (!ok(ort_->CreateCpuMemoryInfo(OrtArenaAllocator,
                                          OrtMemTypeDefault,
                                          &memory_info_),
                "CreateCpuMemoryInfo")) {
            return false;
        }

        const size_t input_plane = input_plane_size();
        const size_t output_plane = output_plane_size();
        input_f32_.resize(input_plane * 3);
        output_raw_f32_.resize(output_plane);
        if (!create_io_tensors()) {
            return false;
        }
        enabled_ = true;
        return true;
    }

    bool is_enabled() const {
        return enabled_;
    }

    int depth_width() const {
        return output_width_;
    }

    int depth_height() const {
        return output_height_;
    }

    bool run_nv12_frame(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* nv12,
        int width,
        int height,
        std::vector<uint16_t>& out_depth
    ) {
        if (!enabled_) {
            return false;
        }
        if (!ensure_staging_texture(device, width, height)) {
            return false;
        }

        context->CopyResource(staging_tex_, nv12);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(staging_tex_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            log_message("Depth readback Map failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        build_model_input_from_nv12(mapped, width, height);
        context->Unmap(staging_tex_, 0);

        const char* input_names[] = {input_name_};
        const char* output_names[] = {output_name_};
        OrtValue* output_values[] = {output_tensor_};
        const OrtValue* input_values[] = {input_tensor_};
        auto start = std::chrono::steady_clock::now();
        OrtStatus* status = ort_->Run(
            session_,
            nullptr,
            input_names,
            input_values,
            1,
            output_names,
            1,
            output_values
        );
        if (!ok(status, "Run")) {
            return false;
        }

        normalize_depth_output_f32(output_raw_f32_.data(), out_depth);

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        ++frame_count_;
        if (frame_count_ <= 3 || frame_count_ % 30 == 0) {
            log_message("Depth NPU frame %llu %.1f ms",
                        static_cast<unsigned long long>(frame_count_), ms);
        }
        return true;
    }

    void shutdown() {
        SAFE_RELEASE(staging_tex_);
        if (ort_) {
            if (input_name_ && allocator_) {
                allocator_->Free(allocator_, input_name_);
            }
            if (output_name_ && allocator_) {
                allocator_->Free(allocator_, output_name_);
            }
            if (output_tensor_) {
                ort_->ReleaseValue(output_tensor_);
            }
            if (input_tensor_) {
                ort_->ReleaseValue(input_tensor_);
            }
            if (memory_info_) {
                ort_->ReleaseMemoryInfo(memory_info_);
            }
            if (session_) {
                ort_->ReleaseSession(session_);
            }
            if (session_options_) {
                ort_->ReleaseSessionOptions(session_options_);
            }
            if (env_) {
                ort_->ReleaseEnv(env_);
            }
        }
        input_name_ = nullptr;
        output_name_ = nullptr;
        memory_info_ = nullptr;
        input_tensor_ = nullptr;
        output_tensor_ = nullptr;
        session_ = nullptr;
        session_options_ = nullptr;
        env_ = nullptr;
        allocator_ = nullptr;
        ort_ = nullptr;
        enabled_ = false;
        staging_width_ = 0;
        staging_height_ = 0;
        frame_count_ = 0;
        input_f32_.clear();
        output_raw_f32_.clear();
        input_type_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        output_type_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        input_width_ = 0;
        input_height_ = 0;
        output_width_ = 0;
        output_height_ = 0;
    }

private:
    bool ok(OrtStatus* status, const char* label) {
        if (!status) {
            return true;
        }
        const char* message = ort_ ? ort_->GetErrorMessage(status) : nullptr;
        log_message("Depth model %s failed: %s", label,
                    message ? message : "(null)");
        if (ort_) {
            ort_->ReleaseStatus(status);
        }
        return false;
    }

    bool validate_model_io() {
        OrtTypeInfo* input_info = nullptr;
        const OrtTensorTypeAndShapeInfo* input_shape_info = nullptr;
        if (!ok(ort_->SessionGetInputTypeInfo(session_, 0, &input_info),
                "SessionGetInputTypeInfo")) {
            return false;
        }
        bool valid = true;
        if (!ok(ort_->CastTypeInfoToTensorInfo(input_info, &input_shape_info),
                "CastTypeInfoToTensorInfo(input)")) {
            valid = false;
        }
        if (valid && !ok(ort_->GetTensorElementType(input_shape_info, &input_type_),
                         "GetTensorElementType(input)")) {
            valid = false;
        }
        size_t input_rank = 0;
        if (valid && !ok(ort_->GetDimensionsCount(input_shape_info, &input_rank),
                         "GetDimensionsCount(input)")) {
            valid = false;
        }
        if (valid && (input_type_ != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                      input_rank != 4)) {
            log_message("Depth model expected FLOAT rank-4 input, got type=%d rank=%zu",
                        static_cast<int>(input_type_), input_rank);
            valid = false;
        }
        std::vector<int64_t> input_dims(input_rank);
        if (valid && !ok(ort_->GetDimensions(input_shape_info,
                                             input_dims.data(),
                                             input_dims.size()),
                         "GetDimensions(input)")) {
            valid = false;
        }
        if (valid && (input_dims[0] != 1 || input_dims[1] != 3 ||
                      input_dims[2] != kRequiredDepthHeight ||
                      input_dims[3] != kRequiredDepthWidth)) {
            log_message("Depth model expected static input shape 1x3x%dx%d, got %lldx%lldx%lldx%lld",
                        kRequiredDepthHeight,
                        kRequiredDepthWidth,
                        static_cast<long long>(input_dims[0]),
                        static_cast<long long>(input_dims[1]),
                        static_cast<long long>(input_dims[2]),
                        static_cast<long long>(input_dims[3]));
            valid = false;
        }
        if (valid) {
            input_height_ = static_cast<int>(input_dims[2]);
            input_width_ = static_cast<int>(input_dims[3]);
        }
        if (input_info) {
            ort_->ReleaseTypeInfo(input_info);
        }
        OrtTypeInfo* output_info = nullptr;
        const OrtTensorTypeAndShapeInfo* output_shape_info = nullptr;
        if (valid && !ok(ort_->SessionGetOutputTypeInfo(session_, 0, &output_info),
                         "SessionGetOutputTypeInfo")) {
            valid = false;
        }
        if (valid && !ok(ort_->CastTypeInfoToTensorInfo(output_info, &output_shape_info),
                         "CastTypeInfoToTensorInfo(output)")) {
            valid = false;
        }
        if (valid && !ok(ort_->GetTensorElementType(output_shape_info, &output_type_),
                         "GetTensorElementType(output)")) {
            valid = false;
        }
        size_t output_rank = 0;
        if (valid && !ok(ort_->GetDimensionsCount(output_shape_info, &output_rank),
                         "GetDimensionsCount(output)")) {
            valid = false;
        }
        if (valid && (output_type_ != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                      output_rank != 4)) {
            log_message("Depth model expected FLOAT rank-4 output, got type=%d rank=%zu",
                        static_cast<int>(output_type_), output_rank);
            valid = false;
        }
        std::vector<int64_t> output_dims(output_rank);
        if (valid && !ok(ort_->GetDimensions(output_shape_info,
                                             output_dims.data(),
                                             output_dims.size()),
                         "GetDimensions(output)")) {
            valid = false;
        }
        if (valid && (output_dims[0] != 1 || output_dims[1] != 1 ||
                      output_dims[2] != kRequiredDepthHeight ||
                      output_dims[3] != kRequiredDepthWidth)) {
            log_message("Depth model expected static output shape 1x1x%dx%d, got %lldx%lldx%lldx%lld",
                        kRequiredDepthHeight,
                        kRequiredDepthWidth,
                        static_cast<long long>(output_dims[0]),
                        static_cast<long long>(output_dims[1]),
                        static_cast<long long>(output_dims[2]),
                        static_cast<long long>(output_dims[3]));
            valid = false;
        }
        if (valid) {
            output_height_ = static_cast<int>(output_dims[2]);
            output_width_ = static_cast<int>(output_dims[3]);
            if (output_width_ != input_width_ || output_height_ != input_height_) {
                log_message("Depth model expected matching input/output H/W, got input=%dx%d output=%dx%d",
                            input_width_, input_height_, output_width_, output_height_);
                valid = false;
            }
        }
        if (output_info) {
            ort_->ReleaseTypeInfo(output_info);
        }
        if (valid) {
            log_message("Depth model IO inputType=%d outputType=%d shape=%dx%d",
                        static_cast<int>(input_type_),
                        static_cast<int>(output_type_),
                        input_width_,
                        input_height_);
        }
        return valid;
    }

    bool create_io_tensors() {
        const int64_t input_shape[] = {1, 3, input_height_, input_width_};
        void* input_data = static_cast<void*>(input_f32_.data());
        size_t input_bytes = input_f32_.size() * sizeof(float);
        if (!ok(ort_->CreateTensorWithDataAsOrtValue(
                    memory_info_,
                    input_data,
                    input_bytes,
                    input_shape,
                    std::size(input_shape),
                    input_type_,
                    &input_tensor_),
                "CreateTensorWithDataAsOrtValue(input)")) {
            return false;
        }

        const int64_t output_shape[] = {1, 1, output_height_, output_width_};
        void* output_data = static_cast<void*>(output_raw_f32_.data());
        size_t output_bytes = output_raw_f32_.size() * sizeof(float);
        if (!ok(ort_->CreateTensorWithDataAsOrtValue(
                    memory_info_,
                    output_data,
                    output_bytes,
                    output_shape,
                    std::size(output_shape),
                    output_type_,
                    &output_tensor_),
                "CreateTensorWithDataAsOrtValue(output)")) {
            return false;
        }
        return true;
    }

    bool ensure_staging_texture(ID3D11Device* device, int width, int height) {
        if (staging_tex_ && staging_width_ == width && staging_height_ == height) {
            return true;
        }
        SAFE_RELEASE(staging_tex_);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(width);
        td.Height = static_cast<UINT>(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = device->CreateTexture2D(&td, nullptr, &staging_tex_);
        if (FAILED(hr)) {
            log_message("CreateTexture2D(depth staging) failed: 0x%08X",
                        static_cast<unsigned>(hr));
            return false;
        }
        staging_width_ = width;
        staging_height_ = height;
        return true;
    }

    static float saturate_float(float value) {
        return std::max(0.0f, std::min(1.0f, value));
    }

    static float normalize_image_channel(float value, int channel) {
        static constexpr float mean[] = {0.485f, 0.456f, 0.406f};
        static constexpr float stddev[] = {0.229f, 0.224f, 0.225f};
        return (saturate_float(value) - mean[channel]) / stddev[channel];
    }

    void build_model_input_from_nv12(
        const D3D11_MAPPED_SUBRESOURCE& mapped,
        int width,
        int height
    ) {
        const auto* y_plane = static_cast<const uint8_t*>(mapped.pData);
        const auto* uv_plane = y_plane + mapped.RowPitch * height;
        const size_t plane_size = input_plane_size();

        for (int y = 0; y < input_height_; ++y) {
            int sy = std::min(height - 1,
                              static_cast<int>((static_cast<double>(y) + 0.5) *
                                               height / input_height_));
            const uint8_t* y_row = y_plane + mapped.RowPitch * sy;
            const uint8_t* uv_row = uv_plane + mapped.RowPitch * (sy / 2);
            for (int x = 0; x < input_width_; ++x) {
                int sx = std::min(width - 1,
                                  static_cast<int>((static_cast<double>(x) + 0.5) *
                                                   width / input_width_));
                int uvx = std::min(width - 2, (sx / 2) * 2);
                float yy = static_cast<float>(y_row[sx]) / 255.0f;
                float cb = static_cast<float>(uv_row[uvx]) / 255.0f - 0.5f;
                float cr = static_cast<float>(uv_row[uvx + 1]) / 255.0f - 0.5f;
                float yv = 1.16438356f * (yy - 0.0625f);
                float r = yv + 1.79274107f * cr;
                float g = yv - 0.21324861f * cb - 0.53290933f * cr;
                float b = yv + 2.11240179f * cb;

                size_t idx = static_cast<size_t>(y) * input_width_ + x;
                input_f32_[idx] = normalize_image_channel(r, 0);
                input_f32_[plane_size + idx] = normalize_image_channel(g, 1);
                input_f32_[plane_size * 2 + idx] = normalize_image_channel(b, 2);
            }
        }
    }

    void normalize_depth_output_f32(const float* output,
                                    std::vector<uint16_t>& out_depth) {
        out_depth.resize(output_plane_size());
        float lo = output[0];
        float hi = output[0];
        for (size_t i = 1; i < out_depth.size(); ++i) {
            float v = output[i];
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        float denom = std::max(hi - lo, 1e-6f);
        for (size_t i = 0; i < out_depth.size(); ++i) {
            float d = (output[i] - lo) / denom;
            d = saturate_float(std::isfinite(d) ? d : 0.0f);
            out_depth[i] = static_cast<uint16_t>(
                std::max(0, std::min(65535, static_cast<int>(std::lround(d * 65535.0f))))
            );
        }
    }

    size_t input_plane_size() const {
        return static_cast<size_t>(input_width_) * static_cast<size_t>(input_height_);
    }

    size_t output_plane_size() const {
        return static_cast<size_t>(output_width_) * static_cast<size_t>(output_height_);
    }

    bool enabled_ = false;
    std::wstring model_path_;
    const OrtApi* ort_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSessionOptions* session_options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtAllocator* allocator_ = nullptr;
    OrtMemoryInfo* memory_info_ = nullptr;
    OrtValue* input_tensor_ = nullptr;
    OrtValue* output_tensor_ = nullptr;
    char* input_name_ = nullptr;
    char* output_name_ = nullptr;
    ID3D11Texture2D* staging_tex_ = nullptr;
    int staging_width_ = 0;
    int staging_height_ = 0;
    uint64_t frame_count_ = 0;
    ONNXTensorElementDataType input_type_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    ONNXTensorElementDataType output_type_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    int input_width_ = 0;
    int input_height_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    std::vector<float> input_f32_;
    std::vector<float> output_raw_f32_;
};

class AcerSrSink {
public:
    ~AcerSrSink() {
        shutdown();
    }

    bool initialize_d3d11(void *device, int source_width, int source_height) {
        if (!device) {
            set_error("D3D11 device is null");
            return false;
        }

        return initialize_common(source_width, source_height,
                                 static_cast<ID3D11Device *>(device));
    }

    bool render_d3d11(void *texture_ptr, unsigned subresource) {
        if (!initialized_) {
            set_error("sink not initialized");
            return false;
        }
        if (!texture_ptr) {
            set_error("invalid D3D11 texture");
            return false;
        }

        pump_messages();

        if (waitable_) {
            WaitForSingleObjectEx(waitable_, 1000, TRUE);
        }

        auto *decoded_tex = static_cast<ID3D11Texture2D *>(texture_ptr);
        d3d_context_->CopySubresourceRegion(nv12_tex_, 0, 0, 0, 0,
                                            decoded_tex, subresource, nullptr);
        if (!run_depth_for_current_frame()) {
            return false;
        }
        convert_nv12_to_sbs_texture();

        bind_and_clear_back_buffer();

        try {
            weaver_->weave();
        } catch (const std::exception& e) {
            set_error("weave failed: %s", e.what());
            return false;
        } catch (...) {
            set_error("weave failed");
            return false;
        }

        HRESULT hr = swap_chain_->Present(1, 0);
        if (FAILED(hr)) {
            set_error("Present failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        return true;
    }

    bool update_depth_u16(const void *data, int width, int height) {
        if (!initialized_) {
            set_error("sink not initialized");
            return false;
        }
        if (!data || width <= 0 || height <= 0) {
            set_error("invalid depth buffer");
            return false;
        }
        if (!create_or_resize_depth_texture(width, height)) {
            return false;
        }

        d3d_context_->UpdateSubresource(
            depth_tex_,
            0,
            nullptr,
            data,
            static_cast<UINT>(width * sizeof(uint16_t)),
            0
        );
        has_depth_ = true;
        return true;
    }

    void shutdown() {
        initialized_ = false;

        if (lens_hint_) {
            try {
                lens_hint_->disable();
            } catch (...) {
            }
            lens_hint_ = nullptr;
        }

        if (weaver_) {
            weaver_->destroy();
            weaver_ = nullptr;
        }

        SAFE_RELEASE(sampler_);
        SAFE_RELEASE(cbuffer_);
        SAFE_RELEASE(ps_);
        SAFE_RELEASE(vs_);
        depth_estimator_.shutdown();
        SAFE_RELEASE(depth_srv_);
        SAFE_RELEASE(depth_tex_);
        SAFE_RELEASE(nv12_uv_srv_);
        SAFE_RELEASE(nv12_y_srv_);
        SAFE_RELEASE(nv12_tex_);
        SAFE_RELEASE(input_rtv_);
        SAFE_RELEASE(input_srv_);
        SAFE_RELEASE(input_tex_);
        SAFE_RELEASE(rtv_);
        SAFE_RELEASE(swap_chain_);
        SAFE_RELEASE(factory_);
        waitable_ = nullptr;
        has_depth_ = false;
        depth_width_ = 0;
        depth_height_ = 0;
        depth_frame_.clear();
        SAFE_RELEASE(d3d_context_);
        SAFE_RELEASE(d3d_device_);

        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        if (sr_context_) {
            SR::SRContext::deleteSRContext(sr_context_);
            sr_context_ = nullptr;
        }
    }

private:
    bool initialize_common(int source_width, int source_height,
                           ID3D11Device *external_device) {
        if (initialized_) {
            return true;
        }

        if (source_width <= 0 || source_height <= 0) {
            set_error("invalid source size %dx%d", source_width, source_height);
            return false;
        }

        source_width_ = source_width;
        source_height_ = source_height;
        render_mode_ = read_render_mode_from_environment();
        stereo_strength_ = read_stereo_strength_from_environment();
        {
            char message[160];
            snprintf(message, sizeof(message),
                     "moonlight-acer-sr: DX11 stereo mode=%s strength=%s\n",
                     render_mode_name(render_mode_),
                     stereo_strength_name(stereo_strength_));
            OutputDebugStringA(message);
        }

        if (!create_sr_context(10.0)) {
            return false;
        }

        if (!get_sr_display_rect(sr_rect_)) {
            set_error("could not resolve SR display rectangle");
            return false;
        }

        sbs_width_ = sr_rect_.right - sr_rect_.left;
        sbs_height_ = sr_rect_.bottom - sr_rect_.top;
        per_eye_width_ = sbs_width_ / 2;
        per_eye_height_ = sbs_height_;
        if (sbs_width_ < 2 || sbs_height_ < 1 || (sbs_width_ % 2) != 0) {
            set_error("invalid SR display size %dx%d", sbs_width_, sbs_height_);
            return false;
        }

        if (!create_window()) {
            return false;
        }

        if (!attach_device(external_device)) {
            return false;
        }

        if (!create_swap_chain()) {
            return false;
        }

        if (!create_input_texture()) {
            return false;
        }

        if (!create_nv12_resources()) {
            return false;
        }

        if (!create_yuv_shader()) {
            return false;
        }
        if (!depth_estimator_.initialize_from_environment()) {
            set_error("Depth model initialization failed");
            return false;
        }

        WeaverErrorCode result =
            SR::CreateDX11Weaver(sr_context_, d3d_context_, hwnd_, &weaver_);
        if (result != WeaverErrorCode::WeaverSuccess || !weaver_) {
            set_error("CreateDX11Weaver failed: code=%d weaver=%p",
                      static_cast<int>(result), static_cast<void *>(weaver_));
            return false;
        }

        try {
            weaver_->enableLateLatching(true);
        } catch (...) {
            // Older runtimes may reject this; continue.
        }

        try {
            weaver_->setShaderSRGBConversion(false, false);
        } catch (...) {
            // Continue with runtime defaults.
        }

        try {
            sr_context_->initialize();
        } catch (const std::exception& e) {
            set_error("SRContext::initialize failed: %s", e.what());
            return false;
        } catch (...) {
            set_error("SRContext::initialize failed");
            return false;
        }

        try {
            lens_hint_ = SR::SwitchableLensHint::create(*sr_context_);
            if (lens_hint_) {
                lens_hint_->enable();
            }
        } catch (...) {
            lens_hint_ = nullptr;
        }

        try {
            weaver_->setInputViewTexture(input_srv_, per_eye_width_,
                                         per_eye_height_, input_srv_format_);
        } catch (const std::exception& e) {
            set_error("setInputViewTexture failed: %s", e.what());
            return false;
        } catch (...) {
            set_error("setInputViewTexture failed");
            return false;
        }

        initialized_ = true;
        return true;
    }

    bool run_depth_for_current_frame() {
        if (!depth_estimator_.is_enabled()) {
            set_error("Depth estimator is not enabled");
            return false;
        }
        if (!depth_estimator_.run_nv12_frame(
                d3d_device_,
                d3d_context_,
                nv12_tex_,
                source_width_,
                source_height_,
                depth_frame_
            )) {
            set_error("Depth inference failed");
            return false;
        }
        if (!update_depth_u16(depth_frame_.data(),
                              depth_estimator_.depth_width(),
                              depth_estimator_.depth_height())) {
            set_error("Depth upload failed");
            return false;
        }
        return true;
    }

    bool create_or_resize_depth_texture(int width, int height) {
        if (depth_tex_ && depth_width_ == width && depth_height_ == height) {
            return true;
        }

        SAFE_RELEASE(depth_srv_);
        SAFE_RELEASE(depth_tex_);
        depth_width_ = 0;
        depth_height_ = 0;
        has_depth_ = false;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(width);
        td.Height = static_cast<UINT>(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &depth_tex_);
        if (FAILED(hr)) {
            set_error("CreateTexture2D(depth) failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R16_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = d3d_device_->CreateShaderResourceView(depth_tex_, &sd, &depth_srv_);
        if (FAILED(hr)) {
            set_error("CreateShaderResourceView(depth) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            SAFE_RELEASE(depth_tex_);
            return false;
        }

        depth_width_ = width;
        depth_height_ = height;
        return true;
    }

    bool create_sr_context(double max_seconds) {
        auto start = std::chrono::steady_clock::now();
        while (!sr_context_) {
            try {
                sr_context_ = SR::SRContext::create();
                return true;
            } catch (const SR::ServerNotAvailableException&) {
                // SR service may still be starting.
            } catch (const std::exception& e) {
                set_error("SRContext::create failed: %s", e.what());
                return false;
            } catch (...) {
                set_error("SRContext::create failed");
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > max_seconds) {
                set_error("SR service not available after %.1fs", max_seconds);
                return false;
            }
        }
        return true;
    }

    bool get_sr_display_rect(RECT& out) {
        SR::IDisplayManager *dm = SR::TryGetDisplayManagerInstance(*sr_context_);
        if (dm) {
            SR::IDisplay *display = dm->getPrimaryActiveSRDisplay();
            if (display && display->isValid()) {
                return fill_rect_from_sr_location(display->getLocation(), out);
            }
            return false;
        }

        SR::Display *display = SR::Display::create(*sr_context_);
        if (display) {
            return fill_rect_from_sr_location(display->getLocation(), out);
        }
        return false;
    }

    bool create_window() {
        HINSTANCE inst = GetModuleHandleA(nullptr);

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "MoonlightAcerSrWindow";
        RegisterClassExA(&wc);

        int x = sr_rect_.left;
        int y = sr_rect_.top;
        int w = sr_rect_.right - sr_rect_.left;
        int h = sr_rect_.bottom - sr_rect_.top;

        hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                wc.lpszClassName,
                                "Moonlight Acer SR",
                                WS_POPUP,
                                x, y, w, h,
                                nullptr, nullptr, inst, nullptr);
        if (!hwnd_) {
            set_error("CreateWindowEx failed: %lu", GetLastError());
            return false;
        }

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    bool attach_device(ID3D11Device *device) {
        d3d_device_ = device;
        d3d_device_->AddRef();
        d3d_device_->GetImmediateContext(&d3d_context_);
        if (!d3d_context_) {
            set_error("external D3D11 device has no immediate context");
            return false;
        }

        IDXGIDevice1 *dxgi_device1 = nullptr;
        if (SUCCEEDED(d3d_device_->QueryInterface(__uuidof(IDXGIDevice1),
                                                  reinterpret_cast<void **>(&dxgi_device1)))) {
            dxgi_device1->SetMaximumFrameLatency(1);
            dxgi_device1->Release();
        }

        IDXGIDevice *dxgi_device = nullptr;
        IDXGIAdapter *adapter = nullptr;
        HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice),
                                                 reinterpret_cast<void **>(&dxgi_device));
        if (SUCCEEDED(hr)) {
            hr = dxgi_device->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = adapter->GetParent(__uuidof(IDXGIFactory2),
                                    reinterpret_cast<void **>(&factory_));
        }
        SAFE_RELEASE(adapter);
        SAFE_RELEASE(dxgi_device);
        if (FAILED(hr)) {
            set_error("could not obtain DXGI factory from external device: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        return true;
    }

    bool create_swap_chain() {
        int w = sr_rect_.right - sr_rect_.left;
        int h = sr_rect_.bottom - sr_rect_.top;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = static_cast<UINT>(w);
        sd.Height = static_cast<UINT>(h);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        HRESULT hr = factory_->CreateSwapChainForHwnd(d3d_device_, hwnd_, &sd,
                                                      nullptr, nullptr,
                                                      &swap_chain_);

        if (FAILED(hr)) {
            set_error("CreateSwapChainForHwnd failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(swap_chain_->QueryInterface(__uuidof(IDXGISwapChain2),
                                                  reinterpret_cast<void **>(&sc2)))) {
            sc2->SetMaximumFrameLatency(1);
            waitable_ = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }

        ID3D11Texture2D *back_buffer = nullptr;
        hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void **>(&back_buffer));
        if (FAILED(hr)) {
            set_error("GetBuffer(backbuffer) failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = d3d_device_->CreateRenderTargetView(back_buffer, &rd, &rtv_);
        back_buffer->Release();
        if (FAILED(hr)) {
            set_error("CreateRenderTargetView failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        output_width_ = w;
        output_height_ = h;
        return true;
    }

    bool create_input_texture() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(sbs_width_);
        td.Height = static_cast<UINT>(sbs_height_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &input_tex_);
        if (FAILED(hr)) {
            set_error("CreateTexture2D(input) failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        input_srv_format_ = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sd.Format = input_srv_format_;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;

        hr = d3d_device_->CreateShaderResourceView(input_tex_, &sd, &input_srv_);
        if (FAILED(hr)) {
            set_error("CreateShaderResourceView(input) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = d3d_device_->CreateRenderTargetView(input_tex_, &rd, &input_rtv_);
        if (FAILED(hr)) {
            set_error("CreateRenderTargetView(input) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    bool create_nv12_resources() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(source_width_);
        td.Height = static_cast<UINT>(source_height_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &nv12_tex_);
        if (FAILED(hr)) {
            set_error("CreateTexture2D(NV12) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC yv = {};
        yv.Format = DXGI_FORMAT_R8_UNORM;
        yv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yv.Texture2D.MipLevels = 1;
        hr = d3d_device_->CreateShaderResourceView(nv12_tex_, &yv, &nv12_y_srv_);
        if (FAILED(hr)) {
            set_error("CreateShaderResourceView(NV12 Y) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC uv = {};
        uv.Format = DXGI_FORMAT_R8G8_UNORM;
        uv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uv.Texture2D.MipLevels = 1;
        hr = d3d_device_->CreateShaderResourceView(nv12_tex_, &uv, &nv12_uv_srv_);
        if (FAILED(hr)) {
            set_error("CreateShaderResourceView(NV12 UV) failed: 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        return true;
    }

    bool compile_shader(const char *source, const char *entry,
                        const char *target, ID3DBlob **blob) {
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ID3DBlob *errors = nullptr;
        HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr,
                                nullptr, entry, target, flags, 0, blob, &errors);
        if (FAILED(hr)) {
            if (errors) {
                set_error("D3DCompile(%s) failed: %.*s", entry,
                          static_cast<int>(errors->GetBufferSize()),
                          static_cast<const char *>(errors->GetBufferPointer()));
                errors->Release();
            } else {
                set_error("D3DCompile(%s) failed: 0x%08X", entry,
                          static_cast<unsigned>(hr));
            }
            return false;
        }
        SAFE_RELEASE(errors);
        return true;
    }

    bool create_yuv_shader() {
        ID3DBlob *vs_blob = nullptr;
        ID3DBlob *ps_blob = nullptr;

        if (!compile_shader(kFullscreenVs, "main", "vs_5_0", &vs_blob)) {
            return false;
        }
        if (!compile_shader(kNv12ToRgbPs, "main", "ps_5_0", &ps_blob)) {
            SAFE_RELEASE(vs_blob);
            return false;
        }

        HRESULT hr = d3d_device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                     vs_blob->GetBufferSize(),
                                                     nullptr, &vs_);
        SAFE_RELEASE(vs_blob);
        if (FAILED(hr)) {
            SAFE_RELEASE(ps_blob);
            set_error("CreateVertexShader failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        hr = d3d_device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                            ps_blob->GetBufferSize(),
                                            nullptr, &ps_);
        SAFE_RELEASE(ps_blob);
        if (FAILED(hr)) {
            set_error("CreatePixelShader failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d3d_device_->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) {
            set_error("CreateSamplerState failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ConvertConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = d3d_device_->CreateBuffer(&bd, nullptr, &cbuffer_);
        if (FAILED(hr)) {
            set_error("CreateBuffer(constants) failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        return true;
    }

    void apply_stereo_strength(ConvertConstants& constants) const {
        switch (stereo_strength_) {
            case kStereoStrengthStrong:
                constants.parallax_balance = 1.00f;
                constants.max_shift_px = 52.0f;
                constants.edge_low_px = 0.6f;
                constants.edge_high_px = 4.8f;
                constants.edge_shift_scale = 0.50f;
                constants.repair_strength = 0.34f;
                constants.repair_low_px = 0.7f;
                constants.repair_high_px = 4.4f;
                break;
            case kStereoStrengthMedium:
                constants.parallax_balance = 0.72f;
                constants.max_shift_px = 36.0f;
                constants.edge_low_px = 0.8f;
                constants.edge_high_px = 5.4f;
                constants.edge_shift_scale = 0.54f;
                constants.repair_strength = 0.24f;
                constants.repair_low_px = 0.9f;
                constants.repair_high_px = 5.0f;
                break;
            case kStereoStrengthWeak:
            default:
                constants.parallax_balance = 0.50f;
                constants.max_shift_px = 16.0f;
                constants.repair_strength = 0.0f;
                break;
        }
    }

    void convert_nv12_to_sbs_texture() {
        FLOAT black[4] = {0, 0, 0, 1};
        d3d_context_->ClearRenderTargetView(input_rtv_, black);
        d3d_context_->OMSetRenderTargets(1, &input_rtv_, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<FLOAT>(sbs_width_);
        vp.Height = static_cast<FLOAT>(sbs_height_);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        d3d_context_->RSSetViewports(1, &vp);

        ConvertConstants constants = {};
        constants.source_width = static_cast<float>(source_width_);
        constants.source_height = static_cast<float>(source_height_);
        constants.per_eye_width = static_cast<float>(per_eye_width_);
        constants.per_eye_height = static_cast<float>(per_eye_height_);
        constants.has_depth = has_depth_ ? 1.0f : 0.0f;
        constants.max_shift_px = 24.0f;
        constants.parallax_balance = 0.50f;
        constants.convergence = 0.50f;
        constants.depth_width = static_cast<float>(std::max(depth_width_, 1));
        constants.depth_height = static_cast<float>(std::max(depth_height_, 1));
        constants.render_mode = static_cast<float>(render_mode_);
        constants.stereo_strength = static_cast<float>(stereo_strength_);
        constants.fg_shift_px = -8.9f;
        constants.mg_shift_px = -2.5f;
        constants.bg_shift_px = 2.9f;
        constants.fg_pop_multiplier = 1.07f;
        constants.bg_push_multiplier = 1.03f;
        constants.depth_mid = 0.45f;
        constants.subject_lock_strength = 0.85f;
        constants.convergence_px = 0.0f;
        constants.edge_low_px = 1.5f;
        constants.edge_high_px = 8.0f;
        constants.edge_shift_scale = 0.58f;
        constants.repair_strength = 0.12f;
        constants.repair_low_px = 2.0f;
        constants.repair_high_px = 7.0f;
        constants.luma_protect_strength = 0.75f;
        apply_stereo_strength(constants);
        d3d_context_->UpdateSubresource(cbuffer_, 0, nullptr, &constants, 0, 0);

        ID3D11ShaderResourceView *srvs[] = {
            nv12_y_srv_,
            nv12_uv_srv_,
            has_depth_ ? depth_srv_ : nullptr,
        };
        d3d_context_->IASetInputLayout(nullptr);
        d3d_context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d_context_->VSSetShader(vs_, nullptr, 0);
        d3d_context_->PSSetShader(ps_, nullptr, 0);
        d3d_context_->PSSetShaderResources(0, 3, srvs);
        d3d_context_->PSSetSamplers(0, 1, &sampler_);
        d3d_context_->PSSetConstantBuffers(0, 1, &cbuffer_);
        d3d_context_->Draw(3, 0);

        ID3D11ShaderResourceView *null_srvs[] = { nullptr, nullptr, nullptr };
        d3d_context_->PSSetShaderResources(0, 3, null_srvs);
        ID3D11Buffer *null_buffers[] = { nullptr };
        d3d_context_->PSSetConstantBuffers(0, 1, null_buffers);
    }

    void bind_and_clear_back_buffer() {
        FLOAT black[4] = {0, 0, 0, 1};
        d3d_context_->ClearRenderTargetView(rtv_, black);
        d3d_context_->OMSetRenderTargets(1, &rtv_, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<FLOAT>(output_width_);
        vp.Height = static_cast<FLOAT>(output_height_);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        d3d_context_->RSSetViewports(1, &vp);
    }

    void pump_messages() {
        MSG msg;
        while (PeekMessageA(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    bool initialized_ = false;
    int source_width_ = 0;
    int source_height_ = 0;
    int sbs_width_ = 0;
    int sbs_height_ = 0;
    int per_eye_width_ = 0;
    int per_eye_height_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    RECT sr_rect_ = {};

    HWND hwnd_ = nullptr;

    SR::SRContext *sr_context_ = nullptr;
    SR::IDX11Weaver1 *weaver_ = nullptr;
    SR::SwitchableLensHint *lens_hint_ = nullptr;

    ID3D11Device *d3d_device_ = nullptr;
    ID3D11DeviceContext *d3d_context_ = nullptr;
    IDXGIFactory2 *factory_ = nullptr;
    IDXGISwapChain1 *swap_chain_ = nullptr;
    ID3D11RenderTargetView *rtv_ = nullptr;
    HANDLE waitable_ = nullptr;

    ID3D11Texture2D *input_tex_ = nullptr;
    ID3D11ShaderResourceView *input_srv_ = nullptr;
    ID3D11RenderTargetView *input_rtv_ = nullptr;
    DXGI_FORMAT input_srv_format_ = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D *nv12_tex_ = nullptr;
    ID3D11ShaderResourceView *nv12_y_srv_ = nullptr;
    ID3D11ShaderResourceView *nv12_uv_srv_ = nullptr;
    ID3D11Texture2D *depth_tex_ = nullptr;
    ID3D11ShaderResourceView *depth_srv_ = nullptr;
    bool has_depth_ = false;
    int depth_width_ = 0;
    int depth_height_ = 0;
    DepthEstimator depth_estimator_;
    std::vector<uint16_t> depth_frame_;
    int render_mode_ = kRenderModeStereo;
    int stereo_strength_ = kStereoStrengthWeak;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11SamplerState *sampler_ = nullptr;
    ID3D11Buffer *cbuffer_ = nullptr;
};

AcerSrSink g_sink;

} // namespace

extern "C" __declspec(dllexport) int
moonlight_acer_sr_init_d3d11(void *device, int source_width, int source_height) {
    g_last_error.clear();
    return g_sink.initialize_d3d11(device, source_width, source_height) ? 1 : 0;
}

extern "C" __declspec(dllexport) int
moonlight_acer_sr_push_d3d11(void *texture, unsigned subresource) {
    g_last_error.clear();
    return g_sink.render_d3d11(texture, subresource) ? 1 : 0;
}

extern "C" __declspec(dllexport) int
moonlight_acer_sr_update_depth_u16(const void *data, int width, int height) {
    g_last_error.clear();
    return g_sink.update_depth_u16(data, width, height) ? 1 : 0;
}

extern "C" __declspec(dllexport) void
moonlight_acer_sr_shutdown(void) {
    g_sink.shutdown();
}

extern "C" __declspec(dllexport) const char *
moonlight_acer_sr_last_error(void) {
    return g_last_error.c_str();
}
