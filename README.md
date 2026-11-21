# ME+

ME+ 是 Windows x64 版 REAPER 的 Media Explorer 扩展合集。当前骨架可以独立编译和加载，功能模块将在收到源码后逐项接入。

## 添加功能

把每个新功能的原始源码放进 [incoming](incoming/) 下的单独目录。Lua 脚本和原有 C++ 工程都可以保留原结构。构建只包含已经接入的模块。

维护者将适配后的代码放入 [features](features/)，在该目录的 `CMakeLists.txt` 中添加源文件，并在 `registry.cpp` 登记模块的加载和卸载函数。模块接口见 [core/feature.h](core/feature.h)。模块通过传入的 REAPER 接口注册动作和获取 API。动作的命名 ID 在后续版本中应保持稳定，以保留用户的快捷键绑定。

加载函数成功时返回 `true`。失败时由该模块先清理自己的部分注册，再返回 `false`；核心会按相反顺序卸载先前成功加载的模块。正常退出时同样逆序清理。

## 构建与安装

在 Windows 上安装 CMake 3.20 或更高版本、Visual Studio 2022 的 C++ 桌面工具和 Windows SDK。在项目根目录运行

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成的插件为 `build/Release/reaper_meplus.dll`。需要安装时，在 REAPER 的 Options 菜单中打开资源目录，退出目标实例，将 DLL 放进该资源目录的 `UserPlugins` 子目录，再启动 REAPER。

项目随附构建所需的 Cockos SDK 头文件，版权和许可保留在文件开头。[REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php)
