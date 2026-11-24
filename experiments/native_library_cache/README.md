# Media Explorer 总库回切缓存原型

扩展在 Windows x64 的 REAPER 7.82 进程内接管「总库」切换中的解析和排序调用。树快捷项、搜索、列表发布和媒体操作由原生 Media Explorer 提供。它目前只缓存资源目录下的 `MediaDB/06.ReaperFileList`。

## 构建与启用

在 ME+ 的仓库根目录运行。

```powershell
cmake -S experiments/native_library_cache -B build/native-library-cache -G "Visual Studio 17 2022" -A x64
cmake --build build/native-library-cache --config Release
cmake --install build/native-library-cache --config Release --prefix build/native-library-cache/package
```

将生成的包解压到独立测试实例的 REAPER 资源目录。DLL 放在 `UserPlugins`，`native_cache_prototype.enable` 放在资源目录根部。启动前把库副本放在 `MediaDB` 并配置原生快捷项。退出后移除启用文件即可停用。原型按宿主版本、原厂 DLL 的 SHA-256 和安装前的调用指令做匹配。

## 缓存行为

第一次打开走原生加载。离开该库时，扩展把原生记录和扫描根目录缓冲区转移到缓存。回切时核对数据库的文件身份、大小和修改时间，再把它们交还给原生加载器。原生代码重新筛选结果。名称排序的配置和输入顺序相同时，可以复用排序结果，其他排序继续由 REAPER 执行。

本机隔离实例使用 1,356,223 条记录时，原生快捷项首次打开的 UI 停顿约 2.87 秒，缓存命中约 0.22 秒。测试中改变库副本的时间戳后，下一次打开重新解析，随后可以再次命中。缓存这份大库时，测试进程在切到小库后仍占用约 1 GiB 私有内存。

ReaScript 可通过 `GetExtState("MEPlusNativeCachePrototype", "status")` 读取加载状态和命中次数。
