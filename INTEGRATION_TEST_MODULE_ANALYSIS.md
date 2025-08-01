# Moonlight Qt 集成测试模块分析

## 概述

本文档分析了 Moonlight Qt 中 Pacer 和 D3D11 渲染器模块的边界、接口和依赖关系，为集成测试的实现提供指导。

## 模块架构分析

### 1. Pacer 模块 (帧节拍器)

**文件位置**: `app/streaming/video/ffmpeg-renderers/pacer/`

**核心类**: `Pacer`

**职责**:
- 管理视频帧的节拍和渲染时序
- 实现垂直同步(VSync)调度
- 维护渲染队列和节拍队列
- 处理帧丢弃逻辑

**关键接口**:
```cpp
// 构造函数
Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

// 核心方法
bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing);
void submitFrame(AVFrame* frame);  // 输入：接收新帧
void signalVsync();               // VSync 信号处理
void renderOnMainThread();        // 主线程渲染
```

**输入/输出**:
- **输入**: AVFrame* 视频帧
- **输出**: 调用 IFFmpegRenderer::renderFrame() 渲染帧
- **协调**: 通过队列管理和线程同步实现帧节拍

### 2. D3D11VA 渲染器模块

**文件位置**: `app/streaming/video/ffmpeg-renderers/d3d11va.h/.cpp`

**核心类**: `D3D11VARenderer : public IFFmpegRenderer`

**职责**:
- 使用 D3D11 硬件加速渲染视频帧
- 管理 D3D11 设备、上下文和资源
- 处理颜色空间转换和像素格式

**关键接口**:
```cpp
// IFFmpegRenderer 接口实现
bool initialize(PDECODER_PARAMETERS params);
bool prepareDecoderContext(AVCodecContext* context, AVDictionary**);
void renderFrame(AVFrame* frame);  // 核心渲染方法
int getRendererAttributes();
int getDecoderCapabilities();
```

**输入/输出**:
- **输入**: AVFrame* 包含 D3D11 纹理的视频帧
- **输出**: 直接渲染到 SDL 窗口的后缓冲区
- **资源管理**: D3D11 设备、交换链、着色器等

## 模块边界和依赖关系

### Pacer 模块依赖

**直接依赖**:
- `IFFmpegRenderer*` - 渲染器接口（需要 stub）
- `PVIDEO_STATS` - 视频统计结构体
- `AVFrame*` - FFmpeg 视频帧结构
- `SDL_Window*` - SDL 窗口句柄
- `IVsyncSource*` - VSync 源接口（需要 stub）

**系统依赖**:
- Qt 框架: `QQueue`, `QMutex`, `QWaitCondition`
- SDL2: 线程管理、事件系统
- Windows API: VSync 相关（可选）

### D3D11VA 渲染器依赖

**直接依赖**:
- `PDECODER_PARAMETERS` - 解码器参数结构体
- `AVCodecContext*` - FFmpeg 编解码上下文
- `AVFrame*` - 包含 D3D11 纹理的视频帧
- `SDL_Window*` - SDL 窗口句柄

**系统依赖**:
- D3D11 API: `ID3D11Device`, `ID3D11DeviceContext`, `IDXGISwapChain`
- Windows ComPtr: 智能指针管理
- FFmpeg: `libavutil/hwcontext_d3d11va.h`

## 集成测试环境初始化策略

### 测试架构设计

```
[集成测试] 
    ├── [Mock 渲染器] (替代真实 D3D11VA)
    ├── [Pacer 实例] (真实实现)
    ├── [Mock VSync 源] (替代系统 VSync)
    └── [测试帧生成器] (生成 AVFrame 数据)
```

### 需要 Stub 的依赖

#### 1. IFFmpegRenderer Mock 实现

**文件**: `tests/mocks/mock_renderer.h/.cpp`

```cpp
class MockFFmpegRenderer : public IFFmpegRenderer {
public:
    // 记录调用的方法和参数
    std::vector<AVFrame*> renderedFrames;
    
    bool initialize(PDECODER_PARAMETERS params) override { return true; }
    void renderFrame(AVFrame* frame) override {
        renderedFrames.push_back(frame);
        // 模拟渲染延迟
    }
    // ... 其他必要方法的 stub 实现
};
```

#### 2. IVsyncSource Mock 实现

**文件**: `tests/mocks/mock_vsync_source.h/.cpp`

```cpp
class MockVsyncSource : public IVsyncSource {
private:
    bool m_async;
    Pacer* m_pacer;
    
public:
    MockVsyncSource(Pacer* pacer, bool async = true) 
        : m_pacer(pacer), m_async(async) {}
    
    bool initialize(SDL_Window* window, int displayFps) override { return true; }
    bool isAsync() override { return m_async; }
    
    // 测试控制方法
    void triggerVsync() { m_pacer->signalVsync(); }
};
```

#### 3. AVFrame 测试数据生成器

**文件**: `tests/utils/frame_generator.h/.cpp`

```cpp
class TestFrameGenerator {
public:
    static AVFrame* createTestFrame(int width, int height, AVPixelFormat format) {
        AVFrame* frame = av_frame_alloc();
        frame->width = width;
        frame->height = height;
        frame->format = format;
        frame->pkt_dts = SDL_GetTicks(); // 时间戳
        // 分配和填充测试数据
        return frame;
    }
};
```

#### 4. SDL 窗口 Mock (简化版)

**文件**: `tests/mocks/mock_sdl_window.h/.cpp`

```cpp
class MockSDLWindow {
public:
    SDL_Window* createTestWindow() {
        // 创建最小化的测试窗口
        return SDL_CreateWindow("Test", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
    }
};
```

### 不需要 Stub 的核心组件

#### 1. 数据结构体
- `VIDEO_STATS` - 可以直接使用真实结构体
- `DECODER_PARAMETERS` - 可以直接使用，填充测试数据

#### 2. Qt 组件
- `QQueue`, `QMutex`, `QWaitCondition` - 使用真实实现进行线程测试

#### 3. 核心算法
- Pacer 的队列管理逻辑 - 使用真实实现
- 帧丢弃算法 - 使用真实实现

## 集成测试用例设计

### 测试初始化模板

```cpp
// tests/integration/pacer_d3d11_integration_test.cpp
class PacerD3D11IntegrationTest {
private:
    MockFFmpegRenderer* m_mockRenderer;
    MockVsyncSource* m_mockVsync;
    Pacer* m_pacer;
    VIDEO_STATS m_videoStats;
    SDL_Window* m_testWindow;
    
public:
    void setUp() {
        // 初始化 SDL
        SDL_Init(SDL_INIT_VIDEO);
        m_testWindow = SDL_CreateWindow("Test", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
        
        // 创建 Mock 对象
        m_mockRenderer = new MockFFmpegRenderer();
        m_videoStats = {}; // 清零统计数据
        
        // 创建 Pacer
        m_pacer = new Pacer(m_mockRenderer, &m_videoStats);
        m_mockVsync = new MockVsyncSource(m_pacer);
        
        // 初始化 Pacer
        m_pacer->initialize(m_testWindow, 60, true);
    }
    
    void tearDown() {
        delete m_pacer;
        delete m_mockRenderer;
        delete m_mockVsync;
        SDL_DestroyWindow(m_testWindow);
        SDL_Quit();
    }
};
```