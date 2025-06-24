#pragma once
#include "ffmpeg-renderers/pacer/pacer.h"
#include <d3d11.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace ConverToStereo {
    void convertToStereo(Pacer* m_Pacer, AVFrame* frame);
    
    bool extractD3D11TextureFromAVFrame(AVFrame* frame, ID3D11Texture2D** ppTexture, int* pSubresourceIndex);
}