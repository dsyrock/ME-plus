# ME+

ME+ 是 REAPER 的 Media Explorer 扩展合集。

- **波形缩略图**在文件列表的 `Waveform` 列绘制波形，并支持点击缩略图定位试听位置。
- **可拖拽音频区段**检测试听素材中的音频区段，按住 Alt 在预览波形上显示区段，再把区段拖入工程。

两个功能都可以在 Media Explorer 的动作列表中搜索 `ME+`，分别开启、关闭和设置。合并版沿用两个独立版的动作 ID 和设置键。Windows 上按 Alt 显示可拖拽区段。macOS 和 Linux 使用预览区底部的音频概览。

## 安装

在 GitHub Releases 下载对应系统和 REAPER 架构的 ZIP。解压后得到 `reaper_ME+.dll`（Windows x64）、`reaper_ME+.dylib`（macOS Apple Silicon 或 Intel）或 `reaper_ME+.so`（Linux x64 或 ARM64）。

在 REAPER 的 Options 菜单中打开资源目录，再退出 REAPER。如果装过独立版或早期合并版，把 `reaper_audio_regions`、`reaper_media_wave_thumbnails` 或 `reaper_meplus` 对应的插件文件移出 `UserPlugins`。将 `reaper_ME+` 插件文件放入 `UserPlugins` 后重启 REAPER。

## 添加功能

把每个新功能的原始源码放进 [incoming](incoming/) 下的单独目录。Lua 脚本和原有 C++ 工程都可以保留原结构。ME+ 仓库和构建只收录已经接入的模块。当前模块源自音频区段项目的 `d2c0263` 和波形缩略图项目的 `3076f77`。

维护者将适配后的代码放入 [features](features/)，在该目录的 `CMakeLists.txt` 中添加源文件，并在 `registry.cpp` 登记模块的加载和卸载函数。模块接口见 [core/feature.h](core/feature.h)。模块通过传入的 REAPER 接口注册动作和获取 API。动作的命名 ID 在后续版本中应保持稳定，以保留用户的快捷键绑定。

加载函数成功时返回 `true`。失败时由该模块先清理自己的部分注册，再返回 `false`；核心会按相反顺序卸载先前成功加载的模块。正常退出时同样逆序清理。

## 构建

在 Windows 上安装 CMake 3.24 或更高版本、Visual Studio 2022 的 C++ 桌面工具和 Windows SDK。在项目根目录运行

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成的插件为 `build/Release/reaper_ME+.dll`。macOS 和 Linux 上使用 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 和 `cmake --build build --config Release`。CMake 会获取固定版本的 WDL。推送 `v*` 标签后，GitHub Actions 会构建并打包各平台版本，全部构建成功后创建 Release。

项目随附构建所需的 Cockos SDK 头文件，版权和许可保留在文件开头。[REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php)
