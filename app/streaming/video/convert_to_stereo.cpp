#include "convert_to_stereo.h"
#include <SDL.h>
#include <d3d11.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

bool ConverToStereo::extractD3D11TextureFromAVFrame(AVFrame* frame, ID3D11Texture2D** ppTexture, int* pSubresourceIndex)
{
    if (!frame || frame->format != AV_PIX_FMT_D3D11) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Frame is not D3D11 format");
        return false;
    }
    
    // AVFrame在D3D11VA中的数据布局：
    // data[0] - ID3D11Texture2D指针  
    // data[1] - 子资源索引（数组纹理的索引）
    *ppTexture = (ID3D11Texture2D*)frame->data[0];
    *pSubresourceIndex = (int)(intptr_t)frame->data[1];
    
    if (!*ppTexture) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No D3D11 texture in frame");
        return false;
    }
    
    (*ppTexture)->AddRef(); // 增加引用计数
    return true;
}

void ConverToStereo::convertToStereo(Pacer* m_Pacer, AVFrame* frame)
{
    // 检查是否为 D3D11 格式的 AVFrame
    if (frame->format == AV_PIX_FMT_D3D11) {
        ID3D11Texture2D* pTexture = nullptr;
        int subresourceIndex = 0;
        
        // 从 AVFrame 提取 D3D11 纹理（显存）
        if (extractD3D11TextureFromAVFrame(frame, &pTexture, &subresourceIndex)) {
            // 获取纹理描述信息
            D3D11_TEXTURE2D_DESC texDesc;
            pTexture->GetDesc(&texDesc);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "Extracted D3D11 texture from AVFrame: %dx%d, Format: %d", 
                       texDesc.Width, texDesc.Height, texDesc.Format);
            
            // 这里可以进一步处理显存中的纹理数据
            // 例如：进行立体声转换、CUDA处理等
            
            // 释放引用
            pTexture->Release();
        }
    }
    
    // 原有逻辑
    m_Pacer->submitFrame(frame);
}