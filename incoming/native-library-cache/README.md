# Media Explorer 大型数据库磁盘缓存

扩展支持已验证的 Windows x64 REAPER 7.82 和 7.86，为大型媒体数据库生成磁盘快照和排序索引。树快捷项、搜索、列表发布和媒体操作由原生 Media Explorer 提供。`.ReaperFileList` 文件达到 50 MiB（52,428,800 字节）时自动启用缓存。小于该分界的库走原生流程。

## 构建与启用

在 ME+ 的仓库根目录运行。源码、构建文件和安装包都放在 `incoming/native-library-cache` 下。

```powershell
cmake -S incoming/native-library-cache -B incoming/native-library-cache/build/current -G "Visual Studio 17 2022" -A x64
cmake --build incoming/native-library-cache/build/current --config Release
cmake --install incoming/native-library-cache/build/current --config Release --prefix incoming/native-library-cache/build/package
```

把生成的 DLL 放在独立测试实例的 `UserPlugins` 并重启。启动前把库副本放在 `MediaDB` 并配置原生快捷项。原型按宿主版本、原厂 DLL 的 SHA-256 和安装前的调用指令做匹配。

插件首次加载后默认启用。在 Main 或 Media Explorer 动作列表中运行 `ME+: Toggle native library cache` 可以切换。两个动作共用一个开关，状态会保留到下次启动。关闭时停止使用磁盘缓存，并释放内存中的排序索引。重新启用后可以继续使用有效的磁盘文件。

## 缓存行为

首次打开由 REAPER 解析原库。扩展将已解析的路径、属性、标签、自定义字段和扫描根目录分块写入资源目录下的 `UserPlugins/MEPlusNativeCache`。每个库的子目录由源文件的规范路径生成，同名库各自保存 `records.snapshot`。之后命中快照时，扩展用原生分配器重建记录，再交回给原生列表。数据块使用 Windows XPRESS 压缩和 SHA-256 校验。文件身份、大小或修改时间变化，以及快照损坏时，扩展回到原生加载并重建对应的缓存。

符合条件的全库排序按排序设置保存在对应库的子目录。当前支持文件名和本机已验证的第 13 号排序列。应用索引前，扩展会核对数据库身份、排序设置和完整输入顺序。筛选结果和其他排序继续走原生逻辑。

7.82 使用原有缓存目录。它会校验早期版本在资源目录根部生成的 `MEPlusNativeCache/06.snapshot` 和 `06.order`，匹配当前数据库和排序设置的文件可以直接命中并迁到新目录。7.86 使用 `UserPlugins/MEPlusNativeCache/7.86`，首次打开大库时会建立自己的缓存。

切离大库后，文件记录和元数据由 REAPER 正常释放。磁盘文件可跨 REAPER 重启复用。测试的 1,356,223 条记录及一种排序占用约 47.2 MiB 磁盘空间，运行期间保留最后命中的一份排序索引。

ReaScript 可通过 `GetExtState("MEPlusNativeCachePrototype", "status")` 读取快照与排序命中次数，以及读取、解压和校验耗时。
