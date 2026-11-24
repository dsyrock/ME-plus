# Media Explorer 总库回切缓存原型

扩展在 Windows x64 的 REAPER 7.82 进程内缓存「总库」的名称排序顺序。树快捷项、解析、搜索、列表发布和媒体操作由原生 Media Explorer 提供。缓存范围是资源目录下的 `MediaDB/06.ReaperFileList`。

## 构建与启用

在 ME+ 的仓库根目录运行。源码、构建文件和安装包都放在 `incoming/native-library-cache` 下。

```powershell
cmake -S incoming/native-library-cache -B incoming/native-library-cache/build/current -G "Visual Studio 17 2022" -A x64
cmake --build incoming/native-library-cache/build/current --config Release
cmake --install incoming/native-library-cache/build/current --config Release --prefix incoming/native-library-cache/build/package
```

把生成的 DLL 放在独立测试实例的 `UserPlugins` 并重启。启动前把库副本放在 `MediaDB` 并配置原生快捷项。原型按宿主版本、原厂 DLL 的 SHA-256 和安装前的调用指令做匹配。

插件首次加载后默认启用。在 Main 或 Media Explorer 动作列表中运行 `ME+: Toggle native library cache` 可以切换。两个动作共用一个开关，状态会保留到下次启动。关闭时释放排序索引，再启用后会在下一次符合条件的加载时重新建立。

## 缓存行为

REAPER 每次打开总库都会解析数据库并建立本次的原生记录。首次按名称排列全库时，扩展把排序结果保存为文件序号。回切时只有数据库的文件身份、大小、修改时间、排序设置和完整输入顺序都匹配，才会按缓存序号排列新记录。筛选结果和其他排序继续走原生逻辑。

切离总库后，文件记录和元数据由 REAPER 正常释放。每条记录在缓存中用一个 32 位序号表示，1,356,223 条记录约占 5.2 MiB。索引保存在进程内存中，退出 REAPER 后会重新建立。总库打开期间的内存占用仍由原生记录决定。

ReaScript 可通过 `GetExtState("MEPlusNativeCachePrototype", "status")` 读取加载状态、索引内存字节数和排序命中次数。
