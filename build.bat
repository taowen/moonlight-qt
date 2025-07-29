copy /y scripts\appveyor\qmake.bat C:\Qt\6.7.3\msvc2019_arm64\bin
copy /y scripts\appveyor\qtpaths.bat C:\Qt\6.7.3\msvc2019_arm64\bin
copy /y scripts\appveyor\target_qt.conf C:\Qt\6.7.3\msvc2019_arm64\bin
set OLDPATH=%PATH%
set PATH=%OLDPATH%;C:\Qt\6.7.3\msvc2019_arm64\bin
call scripts/build-arch.bat release
set PATH=%OLDPATH%;C:\Qt\6.7.3\msvc2019_64\bin
call scripts/build-arch.bat release
call scripts/generate-bundle.bat release