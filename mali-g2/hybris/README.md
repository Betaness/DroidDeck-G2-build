# 进程内 Mali Vulkan（DroidDeck G2，libhybris）

Vortek（`../vortek`）把游戏的每个 Vulkan 调用经 socket 转发给 App 进程里的服务端执行。这个目录是另一条路：
游戏进程（glibc，aarch64，比如 Proton ARM64 里 Wine 的 unix 侧和其中的 DXVK / vkd3d-proton）通过
[libhybris](https://github.com/taowen/libhybris)（arlinux 分支）在**自己进程里**加载设备的
`/vendor/lib64/hw/vulkan.mali.so`，没有转发和服务端。原型在设备上测得每 4 万条命令（录制 + 提交等待）省约 1.8 ms，
另外省下 Vortek 服务端约 16% 个核。

会话设置 `gpuPath` 为 `hybris` 时只有**游戏**走这条路，Steam 客户端（网页界面）仍走 Vortek。没设时玄戒 O3 默认开、其他 SoC 默认关
（`vortek` 关掉）。O3 上半屏鸭科夫标题 82.1±0.1 对 Vortek 75.8±0.2 fps（各 20、30 分钟，同样机身 41–42 °C，只算 120 Hz 窗口），
主线程 10.9 对 11.1 ms/帧，20 分钟里游戏进程内存不涨；更早的 A/B 是 88.2±0.8 对 78.5±1.0。
9451a6f3 曾因 32 位游戏起不来改回默认关：A.V.A.（D3D9，WoW64）建交换链图像失败（`Failed to create image: -1000158000`），
DXVK 映射内存报 `VK_ERROR_OUT_OF_HOST_MEMORY`。原因是 Mali 没有 `VK_EXT_map_memory_placed`，WoW64 的 winevulkan 退到
`VK_EXT_external_memory_host`，而这条路在 Mali 上两处都不通（`tests/wow64_memory_test.c`）；`src/placed.c` 补上了这个扩展，
之后 A.V.A. 能到服务器选择画面、开关 Steam 菜单后继续出图（3 次，每次约 9 分钟），这两个错误不再出现。
x86_64 Proton 的游戏和缺文件时仍自动走 Vortek（见下）。

## 组成

| 文件 | 作用 |
|---|---|
| `build.sh` | 按固定提交拉取 libhybris 和 Halium android-headers，打 `patches/`，编出 libhybris（common、hardware、Vulkan ICD、q 链接器）和 `libdroiddeck_hybris_vk.so`，生成 guest 的目录树和 ICD 清单。在 aarch64 glibc 环境里跑（PalmCode 的 Ubuntu 24.04、CI 的 ubuntu-24.04-arm），glibc 不高于 guest（Arch Linux ARM）即可 |
| `patches/libhybris-0001-static-tls-4096.patch` | `BIONIC_STATIC_TLS_SIZE` 1024 → 4096：`libGLES_mali.so` 的 TLS 段 832 字节、偏移 400，放不进原来的静态 TLS |
| `patches/libhybris-0002-common-exports.patch` | libhybris-common 用版本脚本只导出 `hybris_*`、`android_*` 等自己的入口（和 `my_property_*`）：它被预加载进游戏的每个进程，自带的 `strlcpy`/`strlcat`、`native_handle_*` 等通用符号不能顶替 glibc 的（`nm -D` 可查） |
| `src/icd.c`, `src/icd.h` | guest 加载器看到的 ICD。dlopen 同目录 `lib/libhybris-vulkan-icd.so.0`（libhybris 的 glibc ICD：实例由安卓的 libvulkan 创建，设备和命令直接进 HAL），只拦截需要的入口，其余原样交给它；不包装句柄。有交换链的设备在图形队列族里多要一个队列给 acquire 用，没有空余时用应用的队列并加锁 |
| `src/wsi.c` | X11 WSI：`VK_KHR_surface`、`VK_KHR_xcb_surface`、`VK_KHR_xlib_surface`、`VK_KHR_get_surface_capabilities2`、`VK_KHR_swapchain`。交换链图像是 AHardwareBuffer（`VK_ANDROID_external_memory_android_hardware_buffer` 导入），经 `$VORTEK_PRESENT_SOCKET` 用 Vortek 的零拷贝协议（`../vortek/src/zero_copy.c`，原样编译）交给合成器显示；acquire 把合成器的释放栅栏导入应用的信号量/栅栏（`headless_swapchain.c` 的做法）。是否能走合成器图层由 X 窗口树决定（`client_x11_present.c` 的逻辑：被别的窗口挡住、被父窗口裁剪、带 alpha 时拷贝进窗口，完全被挡时按 10 fps 节流），两份代码合在一个进程里 |
| `src/mali_fixes.c` | Vortek 客户端里属于驱动问题的补丁：去掉 ClipDistance/CullDistance 写入（`../vortek/src/spirv_clip_cull.h`，和 Vortek 共用；不去掉 Mali 编译器在本进程里段错误，见测试）、vkd3d-proton 的 storage texel buffer 对齐、`VK_EXT_vertex_attribute_divisor` 别名；驱动没有、DXVK 又要求的核心特性 fillModeNonSolid、shaderClipDistance、shaderCullDistance 照 Vortek 服务端的做法报成支持，建设备时再关掉（没有它们 DXVK 找不到设备：`Skipping: Device does not support required feature 'fillModeNonSolid'`）。maintenance5/6、GPL、扩展过滤表（`ext_commands.h`）都是 Vortek 协议的需要，原生驱动不用。另外照 Vortek 服务端（`winlator-0018`）按估计的驱动留存量释放命令内存：Mali 对录进「自上次释放以来写过查询的命令池内存」的每次 `vkCmdBeginRendering` 留约 420 字节主机堆（查询写在池里任何一个命令缓冲、哪怕只写过一次都算，见 `../vortek/README.md`），不带 `RELEASE_RESOURCES` 的重置（和 `vkBeginCommandBuffer` 的隐式重置）不还，DXVK 用 0 重置池，不处理游戏进程每分钟涨几十 MB 直到崩溃。默认包装 `vkCreateCommandPool`/`vkDestroyCommandPool`/`vkAllocateCommandBuffers`/`vkFreeCommandBuffers`、`vkBegin/EndCommandBuffer`、`vkCmdBeginRendering(KHR)`、`vkCmdBeginRenderPass(2)`、`vkCmdBeginQuery(IndexedEXT)`、`vkCmdWriteTimestamp(2)`，按池累计估计（`../vortek/src/cmd_memory.h`，无锁查表，只有建/删池和分配/释放命令缓冲加锁）：池的估计到 `DROIDDECK_HYBRIS_CMD_RELEASE_BYTES`（没有时取 `VORTEK_CMD_RELEASE_BYTES`，默认 `16m`）或所有池合计到 `*_CMD_RELEASE_TOTAL_BYTES`（默认 2 倍，且这个池自己至少有较小限额的 1/16）时，下一次重置加 `RELEASE_RESOURCES`；flags 0 的池重置后内存归池，单个命令缓冲的释放不再抵扣；允许单独重置的池在 `vkBeginCommandBuffer` 前先带标志重置；池里有命令缓冲写过查询后，池里所有命令缓冲的 `vkCmdBeginRendering` 都计入，直到池带标志重置；没写过查询的池永不释放；写过查询的池至少每 4096 次重置释放一次（兜底）；显式 `vkResetCommandBuffer` 之后的 `vkBeginCommandBuffer` 不再补释放（内存已归池，释放不掉）；`*_CMD_RELEASE_LOG=1` 每次释放打一行。`DROIDDECK_HYBRIS_CMD_RELEASE_EVERY`（或 `VORTEK_CMD_RELEASE_EVERY`）设成数字时照旧每第 N 次重置释放（1 每次，0 不释放；256 格的表加锁），这时只包装两个重置函数，给设备 A/B 选 rel0/rel32/按估计 |
| `src/placed.c` | Mali 没有的 `VK_EXT_map_memory_placed`，给 WoW64 的 Wine（32 位游戏的映射要在 4 GB 以下）。Wine 在设备有它时用 `vkMapMemory2KHR` 把内存映射到自己预留的低地址，没有时退到 `VK_EXT_external_memory_host`：给每个 `vkCreateImage` 加 `HOST_ALLOCATION_BIT_EXT`（Mali 返回 -1000158000），主机可见内存导入低地址的匿名内存（Mali 只有缓存、非一致的类型能导入，0x7 类型找不到替代，驱动映射在 4 GB 以上，Wine 报 `VK_ERROR_OUT_OF_HOST_MEMORY`）。照 Vortek 客户端和服务端的做法：开了这个扩展的设备上，主机可见内存是 system-uncached 堆的 dma-buf，按应用要的类型导入，placed 映射就是把 dma-buf mmap 到调用者给的地址，解除映射时按 `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` 保留地址范围；这种内存的 flush/invalidate 不进驱动（驱动按自己的映射做，没有映射时空指针崩溃），改成 dma-buf 同步。别的内存、没开扩展的设备（64 位游戏）不经过这里。`DROIDDECK_HYBRIS_MAP_PLACED=0` 隐藏扩展 |
| `src/ahb.c`, `src/compat/android/hardware_buffer.h` | AHardwareBuffer 调用经 libhybris 转到设备的 `libnativewindow.so`（分配走 gralloc 的 binder 服务，和普通 App 一样） |
| `tests/` | 见下 |

环境变量沿用 Vortek 的同名开关：`VORTEK_CMD_RELEASE_EVERY`/`_BYTES`/`_TOTAL_BYTES`/`_LOG`（`DROIDDECK_HYBRIS_CMD_RELEASE_*` 优先）、`VORTEK_PRESENT_SOCKET`、`VORTEK_ZERO_COPY=0`、`VORTEK_AHB_SWAPCHAIN=0`、`VORTEK_MIN_IMAGES`、
`VORTEK_RELEASE_WAIT`、`VORTEK_X11_SHM=0`、`VORTEK_X11_STATS=N`、`VORTEK_KEEP_CLIP_DISTANCE=1`、`VORTEK_STRICT_TEXEL_ALIGNMENT=1`。
日志前缀 `hybris-icd:`、`hybris-wsi:`、`hybris-x11:`（和 `vortek-x11:` 的格式相同）；连接合成器那行沿用 `vortek-server:`。

## guest 里的文件（`build.sh` 的输出）

```
/usr/local/lib/droiddeck-hybris/libdroiddeck_hybris_vk.so
/usr/local/lib/droiddeck-hybris/droiddeck_hybris_icd.aarch64.json
/usr/local/lib/droiddeck-hybris/lib/libhybris-common.so.1      ← 必须 LD_PRELOAD（静态 TLS）
/usr/local/lib/droiddeck-hybris/lib/libhardware.so.2
/usr/local/lib/droiddeck-hybris/lib/libhybris-vulkan-icd.so.0
/usr/local/lib/droiddeck-hybris/lib/libhybris/linker/q.so
/usr/local/share/licenses/droiddeck-hybris/{NOTICE,LGPL-2.1,Apache-2.0,BSD-2-Clause,SOURCES}
```

都是按 soname 命名的实文件（APK 的 assets 存不了符号链接）。`GUEST_DIR` 编进了 libhybris（链接器目录、RUNPATH），
别处测试用 `tests/run.sh`（设 `HYBRIS_LINKER_DIR`、`LD_LIBRARY_PATH`、`LD_PRELOAD`、`VK_ICD_FILENAMES`）。

## App 里怎么接（O3 默认开）

- `session/HybrisGpu.kt`、`SessionPrefs.gpuPath`（`droiddeckctl prefs set --type string session gpuPath hybris` 开，`vortek` 关；删掉回到默认：O3 开，其他关）。
- 打开时 `SessionService` 挂入安卓系统目录（和 `androidSystemBinds` 相同），guest 环境加 `DROIDDECK_GPU_PATH=hybris`（rootfs 里有 ICD 清单时）。
  droiddeck-env 里写 `DROIDDECK_GPU_PATH=vortek` 可整体关掉，game-environment.json 里给单个游戏写也行。
- `droiddeck-game-env` 的 `hybris_gpu`：只对 ARM64 Proton（有 `files/lib/wine/aarch64-unix`）的游戏启动（`waitforexitandrun`）、
  `BL_GPU_MODE=vortek` 时生效（x86_64 Proton 的 Wine 跑在 FEX 里，留在 Vortek），设
  `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` 为本 ICD，把 `libhybris-common.so.1` 放到 `LD_PRELOAD` 最前，
  `VORTEK_PRESENT_SOCKET=$XDG_RUNTIME_DIR/vortek-present`，`GLIBC_TUNABLES` 加 `glibc.rtld.optional_static_tls=16384`
  （启动脚本的 `LD_AUDIT` 让 rtld 在预加载之前就排好静态 TLS，libhybris-common 约 4 KB 的 TLS 段只能从可选余量里分）。缺文件或 `/system/lib64/libvulkan.so` 时退回 Vortek。
- `SessionFiles` 每次会话从 APK 放进 rootfs（有才放）；CI 的 `hybris` 任务（`continue-on-error`）编出后放进 APK。
- 合成器（`waylandcomp/src/vortek_present.c`）现在同时接受最多 7 个零拷贝连接（Vortek 服务端 + 各游戏进程），
  链 id 按连接区分，释放消息发回所属连接。只有 Vortek 服务端一个连接时行为和以前一样：同一进程重连会替换旧连接，
  最后一个连接断开时收起所有 Vortek 图层；还有别的连接时，收起没有窗口在用的图层（包括记账丢失的）。

## 测试（PalmCode 的 Ubuntu 里能跑；需要能读到 /system、/vendor、/apex）

```sh
sh build.sh                        # 第一次要联网
sh tests/run.sh vulkaninfo --summary
sh tests/run.sh /path/to/compute_glibc        # 私有原型的计算测试，结果和 /opt/hybris 原型一致
sh tests/wsi_test.sh               # Xvfb 上：X11 拷贝（BGRA/RGBA）、零拷贝（mock 合成器验像素）、改窗口大小、
                                   # 部分/完全遮挡切换、vkcube 零拷贝（最后一帧在 out/tests/vkcube.ppm）
sh tests/compositor_test.sh        # 合成器自己的 vortek_present.c（SurfaceControl 打桩）同时服务 3 个进程，
                                   # 再用 presenter_probe 测同进程重连和放不进链 key 的 id
glslangValidator -V --vn clip_vert -o out/tests/clip_vert.h tests/clip.vert && cc -Iout/tests -o out/tests/clip_test tests/clip_test.c -lvulkan
sh tests/run.sh out/tests/clip_test           # 写 ClipDistance 的管线；VORTEK_KEEP_CLIP_DISTANCE=1 时驱动段错误
sh tests/frame_leak_test.sh        # ../vortek/tests/frame_leak_test.c 在本进程里（离屏，每帧 16 个渲染过程 + 时间戳查询），
                                   # 量进程自己的堆（首次报告、峰值、稳态）、每千帧释放次数和进程 CPU：池重置、单缓冲重置、
                                   # vkBeginCommandBuffer 隐式重置三种用法；释放关掉时每帧约涨 6.7 KB；每 32 次时平；
                                   # 按估计（默认 16m）有界，每千帧释放 0.4 次（每 32 次是 31.25 次）；4 个池合计上限；
                                   # 无查询不释放；VkRenderPass + 查询走 1024 次重置的兜底；查询只在每个池第一帧写一次
                                   # （FL_QUERY_EVERY=0）时关掉释放照样每帧涨 6.7 KB，按估计有界
cc -O2 -I../vortek/src -o out/tests/cmd_memory_test ../vortek/tests/cmd_memory_test.c -lpthread && out/tests/cmd_memory_test
                                   # 估计逻辑的单元测试（不用 GPU，含 8 线程并发）
cc -o out/tests/wow64_memory_test tests/wow64_memory_test.c -lvulkan && sh tests/run.sh out/tests/wow64_memory_test
                                   # WoW64 winevulkan 对驱动的调用：设备有 placed 映射时把每种主机可见类型映射到 4 GB 以下，
                                   # GPU 拷贝和填充后核对两边的数据；DROIDDECK_HYBRIS_MAP_PLACED=0 时复现 -1000158000 和 4 GB 以上的映射
```

## 只能在设备上测的

- DroidDeck 会话里（DroidDeck 自己的 proot、guest 是 Arch）游戏进程能否加载：Proton/Wine 进程里 `LD_PRELOAD` libhybris-common、
  wine 的地址空间预留和 bionic 链接器能否共存，WoW64 32 位游戏，FEX 在同一进程里。
- 真合成器的显示（SurfaceControl 图层、AFBC 缓冲、释放栅栏），和 Steam 客户端（Vortek）同时出图时的图层叠放和遮挡。
- 帧率和 CPU：和 Vortek 的 A/B。
- 驱动崩溃现在直接带走游戏进程（不再是整个 Vortek 服务端）。

## 许可

`libdroiddeck_hybris_vk.so`：LGPL-2.1 或更高（含 Vortek 的代码）。libhybris：Apache-2.0，链接器（q.so）来自 bionic，BSD-2-Clause（部分 Apache-2.0）；
另有 strlcpy/strlcat（ISC）、integrity_sha256.c（MIT）、bionic_asm_arm64.h（BSD-3-Clause），声明全文在 `NOTICE`。
android-headers：Apache-2.0。运行时加载的安卓系统库和驱动来自设备本身，不随 APK 分发。详见 `NOTICE`。
