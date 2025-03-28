copy /y scripts\appveyor\qmake.bat C:\Qt\6.8.2\msvc2022_arm64\bin
copy /y scripts\appveyor\qtpaths.bat C:\Qt\6.8.2\msvc2022_arm64\bin
copy /y scripts\appveyor\target_qt.conf C:\Qt\6.8.2\msvc2022_arm64\bin
set OLDPATH=%PATH%
set PATH=%OLDPATH%;C:\Qt\5.15.2\msvc2019\bin
call scripts/build-arch.bat release
set PATH=%OLDPATH%;C:\Qt\6.8.2\msvc2022_arm64\bin
call scripts/build-arch.bat release
set PATH=%OLDPATH%;C:\Qt\6.8.2\msvc2022_64\bin
call scripts/build-arch.bat release
call scripts/generate-bundle.bat release