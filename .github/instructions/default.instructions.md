---
applyTo: '**'
---

# Moonlight Qt 构建指南

## 构建环境要求
- Visual Studio 2022 Community Edition (包含 MSVC v143 编译器)
- Qt 6.7.3 (msvc2019_64)
- Windows SDK 10.0.26100.0
- Git

## 构建命令

### 一行 PowerShell 命令构建
```powershell
cd c:\games\moonlight-qt; .\build.ps1 -BuildConfig release
```

### 构建配置选项
- `debug` - 调试版本
- `release` - 发布版本  
- `signed-release` - 签名发布版本（需要代码无未提交更改）

## 构建过程说明

1. **环境检测**: 脚本会自动检测 Visual Studio 安装路径和 Qt 安装路径
2. **架构检测**: 根据 Qt 路径自动检测目标架构 (x64/x86/arm64)
3. **依赖项构建**: 按顺序构建各个模块
   - qmdnsengine (mDNS 引擎)
   - h264bitstream (H.264 比特流解析)
   - soundio (音频 I/O)
   - moonlight-common-c (Moonlight 核心 C 库)
   - AntiHooking (反注入保护)
   - app (主应用程序)

## 构建输出
构建成功后，输出文件位于：
- `build/deploy-x64-release/Moonlight.exe`

## 项目结构
- `app/` - 主应用程序源码
- `libs/` - 第三方库
- `build/` - 构建输出目录
- `scripts/` - 构建脚本


