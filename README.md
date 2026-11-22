# ME+

ME+ 是为 REAPER 媒体浏览器（Media Explorer）添加额外功能的扩展合集。

## 安装

下载对应系统和 REAPER 架构的 ZIP。解压后得到 `reaper_ME+.dll`（Windows x64）、`reaper_ME+.dylib`（macOS Apple Silicon 或 Intel）或 `reaper_ME+.so`（Linux x64 或 ARM64）。[GitHub Releases](https://github.com/dsyrock/ME-plus/releases/latest)

在 REAPER 的 Options 菜单中打开资源目录，再退出 REAPER。如果装过独立版或早期合并版，把 `reaper_audio_regions`、`reaper_media_wave_thumbnails` 或 `reaper_meplus` 对应的插件文件移出 `UserPlugins`。将 `reaper_ME+` 插件文件放入 `UserPlugins` 后重启 REAPER。

## 功能

在 REAPER 动作列表的 **Media Explorer** 分区搜索 `ME+`，可以找到各模块的开关和设置。

### 波形缩略图

在媒体浏览器的 **Details（详细信息）** 视图中，`Waveform` 列会显示各音频文件的波形，便于在列表里快速浏览素材。点击缩略图上的位置，可从该位置开始试听。波形列的位置和宽度会保存。

使用 `ME+: Toggle waveform thumbnails` 开关波形列，或用 `ME+: Customize waveform colors...` 调整普通和选中状态的波形颜色。

### 可拖拽音频区段

选择一个音频文件，功能会按音量和静音间隔检测有声区段，便于把长文件中的单个声音直接拖入工程。

- **Windows** 上按住 Alt，在预览波形中显示区段框。区段间的空隙会压暗，区段内和标尺保留原有亮度。从框内开始拖动，再放到编排区的轨道上。
- **macOS 和 Linux** 使用预览区底部的音频概览，从标出的区段拖到轨道上。

使用 `ME+: Show draggable audio regions` 开关功能，或用 `ME+: Configure audio region detection` 调整音量门限、静音时长与区段两端的留白。

## 构建

在 Windows 上安装 CMake 3.24 或更高版本、Visual Studio 2022 的 C++ 桌面工具和 Windows SDK。在项目根目录运行

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成的插件为 `build/Release/reaper_ME+.dll`。macOS 和 Linux 上使用 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 和 `cmake --build build --config Release`。CMake 会获取固定版本的 WDL。推送 `v*` 标签后，GitHub Actions 会构建并打包各平台版本，全部构建成功后创建 Release。

项目随附构建所需的 Cockos SDK 头文件，版权和许可保留在文件开头。[REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php)
