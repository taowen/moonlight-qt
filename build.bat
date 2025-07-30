call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
cd /d "c:\games\moonlight-qt"

set OLDPATH=%PATH%
set PATH=%OLDPATH%;C:\Qt\6.7.3\msvc2019_64\bin
call "c:\games\moonlight-qt\scripts\build-arch.bat" release