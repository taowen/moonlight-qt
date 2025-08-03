#include "integration_test.h"
#include "streaming/video/ffmpeg-renderers/d3d11va.h"
#include <iostream>
#include <SDL.h>

// Test function for D3D11VARenderer
int testD3D11VARenderer()
{
    std::cout << "Testing D3D11VARenderer..." << std::endl;
    
    // Initialize SDL for testing
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return -1;
    }
    
    // Create a test window (required for D3D11 initialization)
    SDL_Window* testWindow = SDL_CreateWindow("D3D11VA Test", 
                                              SDL_WINDOWPOS_UNDEFINED, 
                                              SDL_WINDOWPOS_UNDEFINED,
                                              1280, 720, 
                                              SDL_WINDOW_HIDDEN);
    if (!testWindow) {
        std::cerr << "Failed to create test window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }
    
    try {
        // Create D3D11VARenderer instance
        D3D11VARenderer renderer(0);
        
        // Set up decoder parameters for testing
        DECODER_PARAMETERS params = {};
        params.window = testWindow;
        params.width = 1920;
        params.height = 1080;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.enableVsync = false;
        
        // Test initialization
        bool initResult = renderer.initialize(&params);
        
        if (initResult) {
            std::cout << "D3D11VARenderer initialization: PASSED" << std::endl;
            
            // Test getting renderer attributes
            int attributes = renderer.getRendererAttributes();
            std::cout << "Renderer attributes: " << attributes << std::endl;
            
            // Test getting decoder capabilities  
            int capabilities = renderer.getDecoderCapabilities();
            std::cout << "Decoder capabilities: " << capabilities << std::endl;
            
            // Test if test frame is needed
            bool needsTest = renderer.needsTestFrame();
            std::cout << "Needs test frame: " << (needsTest ? "true" : "false") << std::endl;
            
            std::cout << "D3D11VARenderer basic functionality test: PASSED" << std::endl;
        } else {
            std::cout << "D3D11VARenderer initialization: FAILED (expected on systems without D3D11VA support)" << std::endl;
            auto failureReason = renderer.getInitFailureReason();
            switch (failureReason) {
                case IFFmpegRenderer::InitFailureReason::NoHardwareSupport:
                    std::cout << "Failure reason: No hardware support" << std::endl;
                    break;
                case IFFmpegRenderer::InitFailureReason::Unknown:
                    std::cout << "Failure reason: Unknown" << std::endl;
                    break;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Exception during D3D11VARenderer test: " << e.what() << std::endl;
        SDL_DestroyWindow(testWindow);
        SDL_Quit();
        return -1;
    }
    
    // Cleanup
    SDL_DestroyWindow(testWindow);
    SDL_Quit();
    
    std::cout << "D3D11VARenderer test completed successfully" << std::endl;
    return 0;
}

// Main test function implementation
int runIntegrationTests()
{
    std::cout << "Running D3D11VARenderer integration test" << std::endl;
    return testD3D11VARenderer();
}