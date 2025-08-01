# Moonlight Qt 开发说明

## 构建命令

我们使用的命令行是 windows git bash，使用如下的 powershell 脚本进行代码构建

```powershell
cd "C:\games\moonlight-qt" && powershell -ExecutionPolicy Bypass -File build.ps1
```

构建产物位于 `build\build-x64-release\` 目录下。

## 运行测试

测试写在 tests\test-main.cpp 文件下

```powershell
cd "C:\games\moonlight-qt" && powershell -ExecutionPolicy Bypass -File run-test.ps1
```


## 文件目录结构

@FILE_LAYOUT.txt