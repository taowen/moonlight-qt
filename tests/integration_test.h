#pragma once

#include <iostream>
#include <memory>
#include <vector>

// Include the real headers for the classes we need
#include "app/streaming/video/ffmpeg-renderers/pacer/pacer.h"
#include "app/streaming/video/ffmpeg-renderers/renderer.h"
#include "app/streaming/video/decoder.h"
#include "app/settings/streamingpreferences.h"

// Forward declarations to avoid complex dependencies during compilation test
struct SDL_Window;
struct AVFrame;
struct AVCodecContext;
struct AVDictionary;

// Use the types already defined in decoder.h

// Forward declaration of interfaces for compilation test
// IFFmpegRenderer and IVsyncSource are already defined in the included headers

class MockD3D11VARenderer : public IFFmpegRenderer {
public:
    MockD3D11VARenderer(int decoderSelectionPass);
    virtual ~MockD3D11VARenderer();
    
    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    void renderFrame(AVFrame* frame) override;
    
private:
    int m_decoderSelectionPass;
};

// Mock FFmpeg Renderer for testing
class MockFFmpegRenderer : public IFFmpegRenderer {
public:
    std::vector<AVFrame*> renderedFrames;
    
    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    void renderFrame(AVFrame* frame) override;
    int getRendererAttributes() override;
    int getDecoderCapabilities() override;
};

// Mock VSync Source for testing
class MockVsyncSource : public IVsyncSource {
private:
    bool m_async;
    Pacer* m_pacer;
    
public:
    MockVsyncSource(Pacer* pacer, bool async = true);
    
    bool initialize(SDL_Window* window, int displayFps) override;
    bool isAsync() override;
    void triggerVsync();
};

// Test Frame Generator - simplified for compilation test
class TestFrameGenerator {
public:
    static bool canCreateTestFrame(int width, int height);
};

// Basic integration test class focused on compilation and basic construction
class PacerD3D11IntegrationTest {
private:
    std::unique_ptr<MockFFmpegRenderer> m_mockRenderer;
    std::unique_ptr<MockVsyncSource> m_mockVsync;
    std::unique_ptr<Pacer> m_pacer;
    std::unique_ptr<MockD3D11VARenderer> m_d3d11Renderer;
    VIDEO_STATS m_videoStats;
    SDL_Window* m_testWindow;
    
public:
    bool setUp();
    void tearDown();
    bool testCompilation();
    bool testPacerConstruction();
    bool testD3D11Construction();
    bool testFrameGeneration();
};

// Main test function
int runIntegrationTests();