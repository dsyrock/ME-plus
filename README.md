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

- **Windows** 上按住 Alt，在预览波形中显示区段框。Alt 单击框内可创建该段的原生预览选区，并自动填入起止时间。从框内开始拖动，可把该段放到编排区的轨道上。区段间的空隙会压暗，区段内和标尺保留原有亮度。
- **macOS** 上按住 Option（⌥），区段框和暗色空隙会直接叠在原生预览波形上。按住 Option 单击区段可创建原生预览选区，从区段内拖向编排区可查看落点预览，松开鼠标即可插入。
- **Linux** 使用预览区底部的音频概览，从标出的区段拖到轨道上。

Windows 和 macOS 上如果读不到当前缩放范围，区段框会按整段文件显示。若框和已放大的波形对不齐，可运行 `Preview: Zoom out full` 恢复全长视图。

使用 `ME+: Show draggable audio regions` 开关功能，或用 `ME+: Configure audio region detection` 调整音量门限、静音时长与区段两端的留白。

### 大型媒体库缓存

在 Windows x64 的 REAPER 7.82 和 7.86 中，为达到 50 MiB 的媒体数据库自动缓存文件记录和排序索引，加快再次打开大型素材库及全库排序。缓存可跨 REAPER 重启复用，数据库更新或缓存失效时会自动重新建立。

使用 `ME+: Toggle native library cache` 开关功能。缓存保存在 REAPER 资源目录的 `UserPlugins/MEPlusNativeCache`。小型数据库继续使用 REAPER 原生加载流程。

## 构建

在 Windows 上安装 CMake 3.24 或更高版本、Visual Studio 2022 的 C++ 桌面工具和 Windows SDK。在项目根目录运行

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成的插件为 `build/Release/reaper_ME+.dll`。macOS 和 Linux 上使用 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 和 `cmake --build build --config Release`。CMake 会获取固定版本的 WDL。推送 `v*` 标签后，GitHub Actions 会构建并打包各平台版本，全部构建成功后创建 Release。

项目随附构建所需的 Cockos SDK 头文件，版权和许可保留在文件开头。[REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php)
