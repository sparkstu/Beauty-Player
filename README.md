Windows 本地视频播放器。发行构建只使用动态链接的 Qt 6 和 FFmpeg 共享库。

## 构建要求

- Windows 10/11 x64
- Visual Studio 2022，安装「使用 C++ 的桌面开发」
- CMake 3.24 或更新版本
- Qt 6.6 或更新版本，MSVC 2022 64-bit 共享库构建（推荐 6.8.3）
- FFmpeg LGPL 共享开发包（`include/`、`lib/`、`bin/`），已提供在
  `third_party/ffmpeg-lgpl/`

Qt 6.8.3 可从 Qt 官方安装器安装，或使用 aqtinstall：

```powershell
pip install aqtinstall
aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:\Qt
```

安装后的根目录形如 `C:\Qt\6.8.3\msvc2022_64`，其中包含 `bin\`、`lib\`、
`include\`。

## 配置

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DQT_ROOT=C:\Qt\6.8.3\msvc2022_64
```

`QT_ROOT` 也可以改用环境变量传入：

```powershell
$env:QT_ROOT = "C:\Qt\6.8.3\msvc2022_64"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

FFmpeg 默认从 `third_party/ffmpeg-lgpl` 查找；换用其他 LGPL 共享构建时指定
`-DFFMPEG_ROOT=<路径>`。配置阶段会检查 FFmpeg 的 DLL 和导入库，并拒绝静态
Qt。

## 构建

```powershell
cmake --build build --config Release
```

生成的文件位于 `build\Release\`。构建完成后，FFmpeg 的 DLL、`licenses\`
目录和 `NOTICE.md` 会自动复制到可执行文件旁边。


`deploy` 调用 `windeployqt`，把 Qt 的共享 DLL、平台插件和 QML 插件复制到
`build\Release\`，同时复制 FFmpeg DLL、许可证文本和 `NOTICE.md`。整个
`build\Release\` 目录即完整运行目录。目标机器还需安装 Microsoft Visual C++
2015–2022 Redistributable (x64)；如果 `windeployqt` 已复制所需的 VC 运行库，
则无需单独安装。

## 验证构建

```powershell
.\build\Release\play.exe
.\build\Release\play.exe --selftest D:\media\sample.mp4
```

第二条命令不打开窗口，退出码为 0 表示自检通过。

## 动态链接与许可证

- Qt 通过 `Qt6*.dll` 动态链接，构建阶段会拒绝静态 Qt。
- FFmpeg 通过 `avcodec-63.dll`、`avformat-63.dll`、`avfilter-12.dll`、
  `avutil-61.dll`、`swresample-7.dll`、`swscale-10.dll` 动态链接，只使用
  对应的导入库，不链接静态 FFmpeg。
- 不要用静态库替换上述 DLL，也不要把 Qt 或 FFmpeg 静态编入可执行文件。
- 对外分发 `build\Release\` 时，必须保留其中的 DLL、`licenses\` 和
  `NOTICE.md`；详细义务和源码获取方式见 `NOTICE.md`。

