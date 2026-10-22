# 第三方组件与许可证

本项目的发行构建以**动态链接**方式使用 Qt 6 和 FFmpeg。对外分发构建产物时，
必须保留本文件、`licenses/` 目录以及所有 DLL，不得把 Qt 或 FFmpeg 静态链接
进程序。

## Qt 6.8.3

- 使用模块：Qt Core、Qt Gui、Qt Quick、Qt Quick Controls 2、Qt Shader Tools
  （构建期工具）。
- 版本说明：6.8.3 为构建验证所用版本；兼容的 Qt 6.6+ 共享构建使用相同的
  LGPLv3 许可条款。
- 许可证：LGPL-3.0-only（Qt 同时提供商业许可、GPL-2.0-only 和
  GPL-3.0-only 作为替代许可；本项目按 LGPL-3.0-only 使用）。
- 许可证文本：`LICENSES/LGPL-3.0-only.txt`、`LICENSES/GPL-3.0-only.txt`。
- 链接方式：动态链接 `Qt6Core.dll`、`Qt6Gui.dll`、`Qt6Qml.dll`、
  `Qt6Quick.dll`、`Qt6QuickControls2.dll` 及其运行库依赖；Qt Shader Tools
  只在构建期使用。构建系统会拒绝静态 Qt。
- 修改情况：未修改 Qt 源代码。
- 替换方式：可以用兼容的 Qt 6.8.x 共享库替换运行目录中的 `Qt6*.dll` 和
  `plugins/`、`qml/` 下的动态模块；程序不校验这些文件的签名或哈希。
- 对应源码：
  - <https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/>
  - <https://code.qt.io/cgit/qt/>
  - 本源码包不包含 Qt 二进制文件。使用 `deploy` 生成发行目录并对外分发时，
    分发者需自行提供 Qt 的对应源码或符合 LGPLv3 要求的书面要约。
- Qt 运行库自身还包含若干第三方组件，其清单见 Qt 安装目录的 `sbom/` 和
  `LICENSES/`；再分发 Qt 运行库时应一并保留相关声明。

## FFmpeg（LGPL 共享构建）

- 版本：`N-127790-g56dbd4cd37`（2026-10-20）。
- 构建配置：`--enable-shared --disable-static --enable-version3`，未启用
  `--enable-gpl` 和 `--enable-nonfree`，不包含 x264/x265 等 GPL 组件。
- 许可证：LGPL-3.0-or-later。
- 许可证文本：`third_party/ffmpeg-lgpl/LICENSE.txt`、
  `LICENSES/LGPL-3.0-only.txt`、`LICENSES/GPL-3.0-only.txt`。
- 链接方式：动态链接以下 DLL，仅使用对应的导入库：
  - `avcodec-63.dll`
  - `avformat-63.dll`
  - `avfilter-12.dll`
  - `avutil-61.dll`
  - `swresample-7.dll`
  - `swscale-10.dll`
- 修改情况：未修改 FFmpeg 源代码或二进制文件。
- 替换方式：可以用 ABI 兼容的 LGPL FFmpeg 共享构建替换上述 DLL；程序不校验
  这些文件的签名或哈希。
- 完整对应源码：`third_party/ffmpeg-lgpl/source/` 中的
  `ffmpeg-56dbd4cd377bc6ac2a59a9d62a3a93875e2297b3.tar.gz`，
  SHA-256 见同目录 `SHA256SUMS`。上游地址：
  <https://github.com/FFmpeg/FFmpeg/tree/56dbd4cd377bc6ac2a59a9d62a3a93875e2297b3>。
- 该二进制由 BtbN/FFmpeg-Builds 的 `win64-lgpl-shared` 配置生成。构建脚本
  快照为 `third_party/ffmpeg-lgpl/build-scripts.tar.gz`，SHA-256 见同目录
  `build-scripts.SHA256`；上游地址：<https://github.com/BtbN/FFmpeg-Builds>。
- FFmpeg DLL 中静态包含的依赖库由上述构建脚本按固定版本获取和编译；其中
  包括软件 H.264 编码器 OpenH264（BSD-2-Clause）。再分发时应同时保留本通知、
  许可证文本和源码获取方式。

## miniaudio 0.11.25

- 用途：音频设备输出。
- 许可证：MIT-0 或公共领域（本项目选择 MIT-0）。
- 许可证文本：`LICENSES/miniaudio-MIT-0.txt`。
- 链接方式：头文件实现，编译进 `play.exe`。miniaudio 不是 LGPL 组件，允许
  静态编译。
