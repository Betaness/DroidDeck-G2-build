# Vortek：Mali 设备上的 Vulkan 转发（DroidDeck G2）

这个目录是 `mali-g2` 分支新增的部分，让没有 Adreno、也没有可用 DRM render node 的
Mali 设备（例如 Mali-G2 / v15，Android 17）也能在 DroidDeck 的 Linux 环境里用上 GPU：

- **客户端** `libvulkan_vortek.so`：glibc aarch64 的 Vulkan ICD，放在 guest 的
  `/usr/local/lib/droiddeck-vortek/`，清单 `vortek_icd.aarch64.json`。它把 Vulkan 调用经
  unix socket + 共享内存环形缓冲区发给服务端；`vkQueuePresentKHR` 时把画面拷进共享缓冲区，
  再用 MIT-SHM 写进 X 窗口（软件呈现，见 `src/client_x11_present.c`）。
- **服务端** `libvortekserver.so`：bionic 可执行文件（名字是 `lib*.so`，安装时会被解压到
  App 的 `nativeLibraryDir`，App 可以直接执行）。它通过导出的 `sphal` 链接器命名空间
  加载 `/vendor/lib64/hw/vulkan.mali.so`（HAL 的 `HMI` → `open("vk0")`），执行客户端发来的调用。
  必须从 `/data` 下运行（`nativeLibraryDir` 满足），链接器才会给它 `sphal` 命名空间。

源码来自上游，按固定提交拉取再打补丁（上游代码不放进本仓库）：

| 仓库 | 提交 |
|---|---|
| https://github.com/brunodev85/vortek | `b1730c5def9b575672e671aee11d79ae7adc63d1` |
| https://github.com/brunodev85/winlator-app | `8077f374ef2d54127b088de9e8cd7b4af14d9c8b`（只稀疏检出 `vortekrenderer/`、`winlator/`） |

## 文件

| 文件 | 作用 |
|---|---|
| `fetch.sh` | 拉取上游到 `work/` 并应用 `patches/` |
| `patches/` | 对上游的修改：socket 路径可配置、X11 呈现钩子、去掉 JNI 的独立服务端钩子、Mali 上 `vkWaitForFences` 的修复、`VK_EXT_graphics_pipeline_library` 的线上结构（特性/属性、库创建信息、库句柄转换，`vortek-0008`/`winlator-0010`；服务端在链接出的管线还在时推迟销毁它用到的库，绕开 Mali 的问题；测试 `tests/gpl_test.c`）；`winlator-0011`：`ResourceMemory_allocate` 每种内存路径（opaque_fd / dmabuf / ahb / ahb 回退 / plain / native image）第一次分配时打日志，外部内存能力查询改用合法的 buffer usage 并打一次日志；实验开关 `VORTEK_NATIVE_IMAGES=1` 让专用于 OPTIMAL 图像的内存走普通 `vkAllocateMemory`（不可映射，`vkMapMemory` 返回 `VK_ERROR_MEMORY_MAP_FAILED`；带 `VkImportMemoryFdInfoKHR` 或 `VkExportMemoryAllocateInfo` 的分配仍走外部内存路径，pNext 只转发 dedicated/allocate flags/opaque capture address 的副本），测试 `tests/dedicated_image_test.c`；`src/headless_swapchain.c` 的 `VORTEK_MIN_IMAGES`（1..6）覆盖交换链 minImageCount）；`vortek-0009`/`winlator-0012`：命令流式发送，命令缓冲录制的命令超过 `VORTEK_CMD_CHUNK` 字节（客户端环境变量，默认关闭；Duckov 上 A/B 开关没有可测差别，所以不默认开启；可写 `32k`）时先用 DroidDeck 请求 `REQUEST_CODE_DD_CMD_CHUNK`（91，见 `src/droiddeck_requests.h`）发给服务端重放，`vkEndCommandBuffer` 只发剩下的部分，服务端的重放和应用的录制重叠；分块发送只尝试拿客户端调用锁（锁忙就继续攒，超过 1 MiB 才等锁）；客户端请求的头和数据一次发布到环形缓冲区（服务端睡着时只唤醒一次），命令批缓冲按 1.5 倍增长；命令缓冲 id 为空的整批丢弃，非空但已失效的 id 和以前一样检测不到；服务端没有 `textureCompressionBC` 时会建纹理解码器（Mali-G2 有 BC，不会建），它推迟的 BC 拷贝在分块重放时就进了队列，可能在该命令缓冲 End/提交之前、甚至被重置后仍被别的线程的 `vkQueueSubmit` 上传，这种设备上应设 `VORTEK_CMD_CHUNK=0`（服务端会打一次日志提醒）；测试 `tests/cmd_stream_test.c`。在 PalmCode 的 proot 里测得每次唤醒约 38 µs，但那个 proot 的 kompat 会拦截 futex，DroidDeck 自己的 proot（`tools/proot` 的 0011）不拦，所以那里测出的分块开销和总收益不代表游戏里的情况，默认值要以 DroidDeck 客户机里的测量为准，例如用 Duckov 对比 `VORTEK_CMD_CHUNK=0` 和默认值） |
| `patches/vortek-0010-*`、`patches/winlator-0014-*` | `VK_KHR_maintenance7`（特性和属性）、`VK_KHR_unified_image_layouts`、`VK_KHR_shader_float_controls2` 的结构走线上协议（Features2、Properties2、DeviceCreateInfo；以前序列化成长度 0 被丢掉，应用读到 false），DXVK 3.1 读到为真才启用它们（深度和模板分开 load/store、图像全程 GENERAL 布局、着色器用 float_controls2）；`VK_KHR_unified_image_layouts` 比自带的 1.4.302 头文件新，定义补在 serializer 里；服务端 `vkCreateDevice` 收到这些特性时打一行日志（`vortek: vkCreateDevice: maintenance7 1, unifiedImageLayouts 1, shaderFloatControls2 1`）；Adreno 上和 `VK_KHR_shader_float_controls` 一起隐藏 `VK_KHR_shader_float_controls2`；测试 `tests/m7ul_test.c` |
| `src/standalone_main.c` | 服务端入口（代替 JNI 的 `main.c` 和 Java 的 `VortekRendererComponent`）：监听 socket、每个连接一个线程、经 `sphal` 加载厂商 HAL |
| `src/headless_swapchain.c` | 代替依赖 Winlator X server 的 `xwindow_swapchain.c`。零拷贝时 `vkAcquireNextImageKHR` 只等合成器归还图像（像 BufferQueue 的 dequeue，稳定后等待超过一帧会打一次日志），不在 CPU 上等释放栅栏：还没信号的释放栅栏临时导入应用的获取信号量/栅栏（`VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`，sync fd），驱动不接受时改为提交一个在 GPU 上等它的空批次，都不行才回到 `poll()`；`VORTEK_RELEASE_WAIT=import`（默认）/`batch`/`cpu` 可强制某条路径做对比。默认 `minImageCount` 为 3（设备上 Duckov 用 `VORTEK_MIN_IMAGES=3` 平均 54.3 → 56.6 fps），`VORTEK_MIN_IMAGES` 仍可覆盖。测试 `tests/zc_acquire_test.c` + `tests/zc_mock_compositor.c`，用 `tests/zc_acquire_test.sh` 在私有 Xvfb 上跑：默认的 `MOCK_RELEASE=early` 量获取延迟（它的释放栅栏在服务端同一队列上本来就有顺序，查不出漏掉的 GPU 等待）；`MOCK_RELEASE=gpu ZC_EXPECT_FAST=0` 是正确性检查，mock 在自己的 Vulkan 设备上跑一个 `MOCK_GPU_MS`（默认 30）的任务，把它的 sync fd 当释放栅栏，图像下次呈现的获取栅栏若比释放栅栏先信号就失败；`ZC_ACQUIRE_FENCE=1`/`2`（信号量加栅栏/只用栅栏）时每次等过获取栅栏都检查 `vkGetFenceStatus` 为 `VK_SUCCESS`、再等一次不超时 |
| `patches/winlator-0013-*` | 驱动有 `VK_KHR_external_semaphore_fd` 时服务端在设备上启用它，供零拷贝交换链把合成器的释放栅栏导入获取信号量；`vkWaitForFences` 导出未信号栅栏的 sync fd 后（导出有重置的副作用，临时导入的载荷会退回永久载荷）把 fd 的副本临时导回同一个栅栏，否则等过的获取栅栏仍读作 `VK_NOT_READY`，再等一次会永远挂住 |
| `patches/winlator-0015-*` | 客户端断开时释放它没销毁的一切（游戏和 DXVK 通常直接退出；以前服务端全留着，几次启动后设备上服务端占 1.07 GB 映射的 dma-buf 加 1.79 GB swap，内存耗尽）。每个连接（`VkContext`）记录自己建的 instance、device，以及每个 device 上的内存（`ResourceMemory`：fd、dma-buf、AHardwareBuffer）、交换链、着色器模块和全部驱动子对象（Mali 的 `vkDestroyDevice` 不回收没销毁的子对象：栅栏留着导入的 sync file，池和管线留着 GPU 映射），正常销毁时移出；客户端每个进程只有一条连接，id 只在返回它的连接上有效，所以按连接归属；销毁一个不属于本连接的对象（重复销毁、别的设备或连接的 id）只打日志（`untracked`）不执行。断开时：请求线程先处理完环里剩下的请求（干净退出时最后那批销毁以前会被丢掉）再结束，然后 status 置负：timeline 等待（`vkWaitSemaphores`）改为每 100 ms 一片，看到连接关闭就返回（以前无限等待的线程让线程池和整个清理永远卡住，连接关不掉），队列里没开始的异步管线创建跳过；`XWindowSwapchain_forgetContext` 之后，每个 device 在每个队列上提交只带栅栏的空批次、最多等 3 s；等不到时把它的每个 timeline 信号量从主机端 signal 到当前值加 `maxTimelineSemaphoreValueDifference`（客户端留下的等待它自己才会 signal 的值的工作就能跑完），再等 1 s；仍然忙的 device 交给一个后台线程，每 1 s（一分钟后每 10 s）查一次那些栅栏，空闲或丢失后照样释放、再销毁 instance 和 context（日志 `client gone, busy devices drained after X s`），然后按依赖顺序销毁剩下的对象（交换链、framebuffer、视图、管线、……、buffer、image、内存）、纹理解码器、管线库引用，再 `vkDestroyDevice`，最后 `vkDestroyInstance`；每次断开打一行 `vortek-server: client gone (fd=N): released 1 devices, 73 memory allocations (1057.6 MiB), 1 swapchains, 93 other objects, 1 instances`。顺带修了每次 `vkWaitSemaphores` 漏掉的请求副本、连接结束时没释放的 ShaderInspector 和额外数据。测试 `tests/leak_test.c`（`exit`：分配后不释放就 `exit()`；`kill`：等 SIGKILL；`clean`：全部按序销毁）和 `tests/leak_test.sh`（对同一个服务端启动 N 次，每次记录服务端的 VmRSS/RssAnon/RssFile/VmSwap、fd 数、线程数和持有的 dma-buf；`LEAK_MOCK=zc_mock_compositor` 时走零拷贝呈现；`LEAK_GPU_STUCK=1` 留下一个等永远没人 signal 的 timeline 值的批次，`LEAK_GPU_BUSY_MS=N` 让队列再忙约 N ms，runner 会等后台释放完成再采样）。64×16 MiB + 8 张 1024² 图像 + 交换链：修复前每次运行服务端多留约 1060 MiB dma-buf（76 个）、约 90 个 fd、4 个线程且连接关不掉，修复后 20 次 VmRSS 稳定在约 59 MB、fd 7、dma-buf 0 |
| `patches/vortek-0012-*`、`patches/winlator-0017-*` | 环形缓冲区的等待者计数不再被清掉：服务端建好两个环、发出 fd 后马上启动请求线程，客户端以前收到 fd 后又对共享头部做一次 `memset`；请求线程若已自旋完、把 `readWaiters` 记成 1 并睡下，这个计数就被清成 0，超时醒来减 1 后变成 0xFFFFFFFF，此后整条连接上它每次睡着计数都是 0，客户端从不唤醒它，每个请求都等满 50 ms 的 futex 超时（游戏 0.1–0.3 fps、CPU/GPU 空闲，或启动几分钟没有窗口，约每 5–10 次启动一次）。客户端改为 `RingBuffer_attach`（只映射，不清零、不重设 status）；两端的等待者递减都不会低于 0，所以即使计数被清过也只损失一次超时，服务端这时打一行 `ring: waiter count was cleared under a sleeping waiter (old client), repaired`。线上布局和协议不变，新旧客户端、服务端可以混用 |
| `patches/winlator-0018-*`、`src/cmd_memory.h` | Mali 驱动的命令缓冲内存泄漏：一个命令缓冲里只要同时写了查询（`vkCmdWriteTimestamp`/`vkCmdWriteTimestamp2`/`vkCmdBeginQuery`，在渲染内外、几个、哪种都一样）和动态渲染，驱动就为每个 `vkCmdBeginRendering` 留下 415–421 字节主机内存（与其中的绘制数无关），只有带 `RELEASE_RESOURCES` 的重置、释放或销毁池才归还，flags 为 0 的重置和 `vkBeginCommandBuffer` 的隐式重置永不归还（不经 Vortek 直接在设备上跑也一样）。不留的：只有 `vkCmdResetQueryPool` 没写查询、查询在另一个命令缓冲、`VkRenderPass` 的 `vkCmdBeginRenderPass(2)`。DXVK 用 flags 0 重置命令池，游戏的 D3D11 查询和渲染录在同一个命令缓冲里，所以鸭科夫的服务端 Native Heap 每分钟涨约 85 MB，约 9.4 万帧后 Scudo 映射失败（`internal map failure`，栈在 `vkBeginCommandBuffer` 里 Mali 的 malloc）。服务端默认按估计的驱动留存量释放（`src/cmd_memory.h`，和 `../hybris` 共用）：重放命令批时数 `vkCmdBeginRendering` 和查询写入，`vkEndCommandBuffer` 时两者都有就给该命令缓冲和它的池记 420 字节 × 次数；池的估计到 `VORTEK_CMD_RELEASE_BYTES`（默认 `16m`，可写 `512k`、`1g`、字节数）或这个连接所有池合计到 `VORTEK_CMD_RELEASE_TOTAL_BYTES`（默认 2 倍）时，下一次 `vkResetCommandPool`/`vkResetCommandBuffer` 加 `RELEASE_RESOURCES`；允许单独重置的池里该释放的命令缓冲在 `vkBeginCommandBuffer` 前先带标志重置；没写过查询的池永不释放；`VkRenderPass` 加查询的池按兜底每 1024 次重置释放一次；表满了退回每 32 次。`VORTEK_CMD_RELEASE_LOG=1` 每次释放打一行（默认前 4 次和每第 64 次）。设了 `VORTEK_CMD_RELEASE_EVERY` 时照旧每第 N 次重置释放（1 = 每次，0 = 关闭；每连接 64 项的表，新句柄或冲突时也释放），不做统计，供设备 A/B 选 rel0/rel32/按估计。本地（PalmCode proot，设备同时在跑游戏，噪声大）：类 DXVK 帧（16 个渲染通道 + 时间戳）按估计每千帧释放 0.33–0.4 次，每 32 次是 31.25 次；堆封顶在基线 + 2 × 16 MiB；不带释放的重置约 10–15 µs，带释放 30–70 µs（留存几 KB）、约 70 µs（200 KB）、约 240 µs（8 MB）、约 540 µs（17 MB）；释放后的下一次录制多 30–130 µs（小）到 0.5–0.7 ms（8–17 MB），所以每帧摊下来：每次释放 55–190 µs/帧，每 32 次 4–6 µs/帧，16 MiB 约 0.5 µs/帧，代价是一次 1–1.4 ms 的尖峰。顺带修了 `vkWaitSemaphores` 和异步管线创建每次漏掉的 32 字节（无底层缓冲的 MemoryPool 在 `vt_free` 后没释放分配列表的数组）。测试 `tests/frame_leak_test.c`（类似 DXVK 的帧：timeline、`vkQueueSubmit2`、动态渲染、描述符、查询、呈现，`FL_STEPS`/`FL_PASSES`/`FL_QUERY_*`（含 `reset`）/`FL_RENDERPASS=1/2` 等开关用来二分；`FL_POOL_RELEASE=N` 由测试自己每第 N 次重置释放、`FL_TIMING=1` 打出带/不带释放的重置和随后录制的耗时；用 NDK 的 clang（`--target=aarch64-linux-android30`）编成 bionic 程序时不经 Vortek 直接跑设备驱动并打印本进程的堆）、`tests/frame_leak_test.sh`（默认 16 个渲染通道加一个时间戳，报告服务端堆的首次值、峰值、稳态、每千帧释放次数和服务端每帧 CPU；`FL_EXPECT=bounded`（默认：后一半的峰值比前一半每帧涨不超过 `FL_MAX_GROWTH` 64 字节）/`flat`/`grow`，见 `tests/frame_leak_report.awk`）和 `tests/cmd_memory_test.c`（估计逻辑的单元测试，不用 GPU）。设备上：修复前 6778 字节/帧，每 32 次 0；`VORTEK_CMD_RELEASE_EVERY=0` 时 6729 字节/帧 |
| `src/client_x11_present.c/.h` | 客户端的软件 X11 呈现（MIT-SHM / PutImage） |
| `src/droiddeck_defaults.h` | 客户端默认 socket 路径 `/run/droiddeck/vortek/V0` |
| `src/spirv_clip_cull.h` | 去掉着色器里 ClipDistance/CullDistance 写入（Mali 编译器遇到会崩），客户端（`droiddeck_maint.c`）和进程内路径（`../hybris`）共用 |
| `src/zero_copy.c/.h` | 零拷贝出图的线上协议（发给合成器），服务端和进程内路径（`../hybris`）共用；合成器同时接受多个连接（Vortek 服务端和各游戏进程） |
| `build-client.sh` | 在 aarch64 Linux 上编译客户端和 ICD 清单（CI：`ubuntu-24.04-arm`） |
| `build-server.sh` | 用 Android NDK 编译服务端（CI：`build` 任务里的 NDK） |
| `NOTICE`、`LICENSE.LGPL-2.1` | 许可说明，随 APK 放进 guest 的 `/usr/local/share/licenses/droiddeck-vortek/` |

## CI

`.github/workflows/build.yml`：

1. `vortek` 任务（`ubuntu-24.04-arm`）：`fetch.sh` + `build-client.sh`，上传 `vortek-client`。
2. `build` 任务：下载 `vortek-client` 到 `app/src/main/assets/linuxfs/usr/local/lib/droiddeck-vortek/`，
   用 NDK 编译服务端到 `app/src/main/jniLibs/arm64-v8a/libvortekserver.so`（`.gitignore` 里），再照常编 APK。

本地编译：

```sh
./fetch.sh
./build-client.sh out/client                       # aarch64 Linux，需要 cmake libx11-dev libxcb1-dev libxcb-shm0-dev libvulkan-dev
NDK=/path/to/ndk ./build-server.sh out/libvortekserver.so
```

## App 里怎么用（`[mali-g2]` 标记的改动）

- `session/VortekServerComponent.kt`：没有 Adreno、`/vendor/lib64/hw/` 里有 Vulkan HAL、APK 里有服务端时，
  会话开始前作为 `SessionPart` 用 `HostProcess` 启动服务端，socket 在 `filesDir/.wayland-rt/vortek/V0`
  （guest 里是 `/run/droiddeck/vortek/V0`），输出写进会话日志目录的 `vortek.log`，会话结束时停止。
  droiddeck-env 里的 `VORTEK_*` 变量（`VORTEK_VK_MAX_VERSION`、`VORTEK_RESOURCE_MEMORY_TYPE` 等）也传给服务端。
- `session/SessionService.kt`：给 guest 设 `BL_GPU_MODE=vortek`、`VORTEK_SERVER_PATH`、
  `BL_VK_DRIVER`/`VK_ICD_FILENAMES`/`VK_DRIVER_FILES`（指向 Vortek 的 ICD）。droiddeck-env 里写
  `BL_GPU_MODE=` 加别的值可以关掉。
- `session/SessionFiles.kt`：每次会话把客户端、清单和许可文件放进 rootfs。
- `droiddeck-session`：`BL_GPU_MODE=vortek` 时不启动 gamescope，改为启动 rootful Xwayland
  （`-shm`，不用 glamor；支持时加 `-fullscreen`；`BL_XWAYLAND_ARGS` 可追加参数），
  直接作为 App 合成器的客户端；`WAYLAND_DISPLAY` 清空，所有程序走 X11。Steam 的 CEF 用软件渲染
  （`BL_CEF_GPU=1` 可改回 GPU）。Xwayland 的日志在会话日志目录的 `xwayland.log`。
- `gpu/TurnipDriver.java`：没有 Adreno 但有厂商 Vulkan HAL 时，合成器用系统 Vulkan 驱动。
- `core/DeviceSupport.kt`、`MainActivity.kt`：这类设备不再弹「不是 Adreno」的对话框。
- `app/build.gradle`：包名 `com.droiddeck.launcher.g2`，应用名「DroidDeck G2」，可以和官方版并存。

## 限制

- 呈现是软件路径（GPU → 共享缓冲区 → MIT-SHM → Xwayland → wl_shm → 合成器），多次拷贝，没有垂直同步。
- 没有 gamescope：没有 Steam 游戏模式的合成、缩放和性能浮层。
- Vortek 客户端只支持 X11（xlib/xcb）surface，不支持 Wayland surface。
- Steam Linux Runtime 容器（pressure-vessel）里能不能看到 `/run/droiddeck/vortek/V0` 和这个 ICD 还没验证。

## 许可

Vortek 客户端和 vortekrenderer 由 brunodev85 以 LGPL-2.1 发布。`patches/` 是对上游的修改，`src/` 里的新文件
同样以 LGPL-2.1（或更新版本）发布，文件头保留了相应声明。全文见 `LICENSE.LGPL-2.1`，来源说明见 `NOTICE`。
