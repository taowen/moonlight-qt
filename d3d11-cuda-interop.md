基于您提供的代码和D3D11VA渲染器实现，您需要从AVFrame中提取D3D11纹理并使用CUDA互操作API。以下是完整的解决方案：

## 1. 从AVFrame提取D3D11纹理

```cpp
bool extractD3D11TextureFromAVFrame(AVFrame* frame, ID3D11Texture2D** ppTexture, int* pSubresourceIndex) {
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
```

## 2. 将D3D11纹理导入CUDA

```cpp
struct CudaD3D11Resource {
    cudaExternalMemory_t extMemory;
    cudaArray_t cudaArray;
    cudaSurfaceObject_t surfaceObject;
    cudaTextureObject_t textureObject;
    bool isValid;
    
    CudaD3D11Resource() : extMemory(nullptr), cudaArray(nullptr), 
                          surfaceObject(0), textureObject(0), isValid(false) {}
};

bool importD3D11TextureToCuda(ID3D11Texture2D* pTexture, int subresourceIndex, 
                             CudaD3D11Resource& cudaResource) {
    HRESULT hr;
    cudaError_t cudaStatus;
    
    // 1. 获取纹理描述
    D3D11_TEXTURE2D_DESC texDesc;
    pTexture->GetDesc(&texDesc);
    
    // 2. 获取共享句柄
    ComPtr<IDXGIResource1> pDXGIResource;
    hr = pTexture->QueryInterface(__uuidof(IDXGIResource1), (void**)&pDXGIResource);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get DXGI resource interface");
        return false;
    }
    
    HANDLE sharedHandle;
    hr = pDXGIResource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get shared handle");
        return false;
    }
    
    // 3. 设置CUDA外部内存描述符
    cudaExternalMemoryHandleDesc externalMemoryHandleDesc = {};
    externalMemoryHandleDesc.type = cudaExternalMemoryHandleTypeD3D11Resource;
    externalMemoryHandleDesc.handle.win32.handle = sharedHandle;
    externalMemoryHandleDesc.size = 0; // D3D11资源自动计算大小
    
    // 4. 导入外部内存
    cudaStatus = cudaImportExternalMemory(&cudaResource.extMemory, &externalMemoryHandleDesc);
    if (cudaStatus != cudaSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to import external memory: %s", 
                     cudaGetErrorString(cudaStatus));
        return false;
    }
    
    // 5. 创建CUDA数组
    cudaExternalMemoryMipmappedArrayDesc mipmapDesc = {};
    mipmapDesc.offset = 0;
    mipmapDesc.arrayDesc.width = texDesc.Width;
    mipmapDesc.arrayDesc.height = texDesc.Height;
    mipmapDesc.arrayDesc.depth = 1;
    
    // 根据D3D11格式设置CUDA格式
    switch (texDesc.Format) {
        case DXGI_FORMAT_NV12:
            // NV12格式需要特殊处理，这里简化为R8格式
            mipmapDesc.arrayDesc.format = cudaChannelFormatKindUnsigned;
            mipmapDesc.arrayDesc.numChannels = 1;
            mipmapDesc.arrayDesc.w = 8;
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            mipmapDesc.arrayDesc.format = cudaChannelFormatKindUnsigned;
            mipmapDesc.arrayDesc.numChannels = 4;
            mipmapDesc.arrayDesc.w = 8;
            mipmapDesc.arrayDesc.x = 8;
            mipmapDesc.arrayDesc.y = 8;
            mipmapDesc.arrayDesc.z = 8;
            break;
        case DXGI_FORMAT_R16G16_UNORM:  // P010格式的UV平面
            mipmapDesc.arrayDesc.format = cudaChannelFormatKindUnsigned;
            mipmapDesc.arrayDesc.numChannels = 2;
            mipmapDesc.arrayDesc.w = 16;
            mipmapDesc.arrayDesc.x = 16;
            break;
        default:
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unsupported D3D11 format: %d", texDesc.Format);
            cudaDestroyExternalMemory(cudaResource.extMemory);
            return false;
    }
    
    mipmapDesc.arrayDesc.flags = cudaArraySurfaceLoadStore;
    mipmapDesc.numLevels = 1;
    
    cudaMipmappedArray_t mipmappedArray;
    cudaStatus = cudaExternalMemoryGetMappedMipmappedArray(&mipmappedArray, 
                                                          cudaResource.extMemory, 
                                                          &mipmapDesc);
    if (cudaStatus != cudaSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get mapped mipmapped array: %s", 
                     cudaGetErrorString(cudaStatus));
        cudaDestroyExternalMemory(cudaResource.extMemory);
        return false;
    }
    
    // 6. 获取CUDA数组（第0级mipmap）
    cudaStatus = cudaGetMipmappedArrayLevel(&cudaResource.cudaArray, mipmappedArray, 0);
    if (cudaStatus != cudaSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get mipmapped array level: %s", 
                     cudaGetErrorString(cudaStatus));
        cudaFreeMipmappedArray(mipmappedArray);
        cudaDestroyExternalMemory(cudaResource.extMemory);
        return false;
    }
    
    // 7. 创建表面对象（用于写入）
    cudaResourceDesc surfResDesc = {};
    surfResDesc.resType = cudaResourceTypeArray;
    surfResDesc.res.array.array = cudaResource.cudaArray;
    
    cudaStatus = cudaCreateSurfaceObject(&cudaResource.surfaceObject, &surfResDesc);
    if (cudaStatus != cudaSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create surface object: %s", 
                     cudaGetErrorString(cudaStatus));
    }
    
    // 8. 创建纹理对象（用于读取）
    cudaTextureDesc texResDesc = {};
    texResDesc.addressMode[0] = cudaAddressModeClamp;
    texResDesc.addressMode[1] = cudaAddressModeClamp;
    texResDesc.filterMode = cudaFilterModeLinear;
    texResDesc.readMode = cudaReadModeElementType;
    texResDesc.normalizedCoords = 1;
    
    cudaStatus = cudaCreateTextureObject(&cudaResource.textureObject, &surfResDesc, &texResDesc, nullptr);
    if (cudaStatus != cudaSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create texture object: %s", 
                     cudaGetErrorString(cudaStatus));
    }
    
    cudaResource.isValid = true;
    return true;
}
```

## 3. 同步机制（如果支持Keyed Mutex）

```cpp
bool setupD3D11CudaSync(ID3D11Texture2D* pTexture, cudaExternalSemaphore_t& extSemaphore) {
    HRESULT hr;
    
    // 尝试获取Keyed Mutex接口
    ComPtr<IDXGIKeyedMutex> pKeyedMutex;
    hr = pTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&pKeyedMutex);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Texture doesn't support keyed mutex");
        return false;
    }
    
    // 获取共享句柄
    ComPtr<IDXGIResource1> pDXGIResource;
    hr = pKeyedMutex->QueryInterface(__uuidof(IDXGIResource1), (void**)&pDXGIResource);
    if (FAILED(hr)) {
        return false;
    }
    
    HANDLE sharedHandle;
    hr = pDXGIResource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr)) {
        return false;
    }
    
    // 导入到CUDA
    cudaExternalSemaphoreHandleDesc extSemDesc = {};
    extSemDesc.type = cudaExternalSemaphoreHandleTypeKeyedMutex;
    extSemDesc.handle.win32.handle = sharedHandle;
    
    cudaError_t cudaStatus = cudaImportExternalSemaphore(&extSemaphore, &extSemDesc);
    return cudaStatus == cudaSuccess;
}
```

## 4. 使用示例

```cpp
// 使用示例
void processFrameWithCuda(AVFrame* frame) {
    ID3D11Texture2D* pTexture = nullptr;
    int subresourceIndex = 0;
    
    // 1. 从AVFrame提取D3D11纹理
    if (!extractD3D11TextureFromAVFrame(frame, &pTexture, &subresourceIndex)) {
        return;
    }
    
    // 2. 导入到CUDA
    CudaD3D11Resource cudaResource;
    if (!importD3D11TextureToCuda(pTexture, subresourceIndex, cudaResource)) {
        pTexture->Release();
        return;
    }
    
    // 3. 设置同步（可选）
    cudaExternalSemaphore_t extSemaphore = nullptr;
    bool hasSync = setupD3D11CudaSync(pTexture, extSemaphore);
    
    // 4. 在CUDA中处理数据
    // 注意：需要适当的同步
    if (hasSync) {
        // 等待D3D11完成
        cudaExternalSemaphoreWaitParams waitParams = {};
        waitParams.params.keyedMutex.key = 0; // 适当的键值
        cudaWaitExternalSemaphoresAsync(&extSemaphore, &waitParams, 1, 0);
    }
    
    // 执行CUDA内核
    // processTextureKernel<<<grid, block>>>(cudaResource.surfaceObject, ...);
    
    if (hasSync) {
        // 通知D3D11 CUDA处理完成
        cudaExternalSemaphoreSignalParams signalParams = {};
        signalParams.params.keyedMutex.key = 1; // 适当的键值
        cudaSignalExternalSemaphoresAsync(&extSemaphore, &signalParams, 1, 0);
    }
    
    // 5. 清理资源
    if (extSemaphore) {
        cudaDestroyExternalSemaphore(extSemaphore);
    }
    
    if (cudaResource.isValid) {
        if (cudaResource.textureObject) cudaDestroyTextureObject(cudaResource.textureObject);
        if (cudaResource.surfaceObject) cudaDestroySurfaceObject(cudaResource.surfaceObject);
        if (cudaResource.extMemory) cudaDestroyExternalMemory(cudaResource.extMemory);
    }
    
    pTexture->Release();
}
```

## 关键要点：

1. **格式处理**：不同的视频格式（NV12, P010, AYUV等）需要不同的CUDA格式映射
2. **同步**：使用Keyed Mutex或其他同步机制确保D3D11和CUDA之间的正确同步
3. **内存管理**：正确管理引用计数和CUDA资源生命周期
4. **错误处理**：充分的错误检查和资源清理

这个解决方案允许您直接访问AVFrame中的D3D11纹理数据，无需额外的内存拷贝，实现零拷贝的GPU间数据传递。