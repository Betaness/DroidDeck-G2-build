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
| `patches/vortek-0013-*`、`patches/winlator-0021-*`、`tests/ring_test.c` | 比环形缓冲区大的传输分段走：服务端用 256 KB 的回复环回应，应用自己的 VkPipelineCache 超过 256 KB 时（或别的超过 256 KB 的回复）`vkGetPipelineCacheData` 以前在服务端写完头后失败、客户端读时报 `ring: buffer overflow on read (2295918/262144)`，返回 `VK_ERROR_DEVICE_LOST`，连接从此错位。现在两边都按最多半个环分段读写，对方同时搬剩下的（读的一方等半个环的数据、写的一方等半个环的空间，加起来不超过一个环，不会互等）。客户端和服务端来自同一个 APK，两边都要有。`tests/ring_test.c`：服务端的 `ring_buffer.c` 建两个 256 KB 环、另一个进程用客户端的 `ring_buffer.c` 接上，8 字节到 3 MB 的消息两个方向逐字节核对（构建命令在文件头；修复前 262145 字节那条就失败）；`tests/pcache_test.sh` 第 11 步：应用自带 1.3 MB 缓存经 Vortek 保存、再读回、热启动 |
| `src/standalone_main.c` | 服务端入口（代替 JNI 的 `main.c` 和 Java 的 `VortekRendererComponent`）：监听 socket、每个连接一个线程、经 `sphal` 加载厂商 HAL |
| `src/headless_swapchain.c` | 代替依赖 Winlator X server 的 `xwindow_swapchain.c`。零拷贝时 `vkAcquireNextImageKHR` 只等合成器归还图像（像 BufferQueue 的 dequeue，稳定后等待超过一帧会打一次日志），不在 CPU 上等释放栅栏：还没信号的释放栅栏临时导入应用的获取信号量/栅栏（`VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`，sync fd），驱动不接受时改为提交一个在 GPU 上等它的空批次，都不行才回到 `poll()`；`VORTEK_RELEASE_WAIT=import`（默认）/`batch`/`cpu` 可强制某条路径做对比。默认 `minImageCount` 为 3（设备上 Duckov 用 `VORTEK_MIN_IMAGES=3` 平均 54.3 → 56.6 fps），`VORTEK_MIN_IMAGES` 仍可覆盖。测试 `tests/zc_acquire_test.c` + `tests/zc_mock_compositor.c`，用 `tests/zc_acquire_test.sh` 在私有 Xvfb 上跑：默认的 `MOCK_RELEASE=early` 量获取延迟（它的释放栅栏在服务端同一队列上本来就有顺序，查不出漏掉的 GPU 等待）；`MOCK_RELEASE=gpu ZC_EXPECT_FAST=0` 是正确性检查，mock 在自己的 Vulkan 设备上跑一个 `MOCK_GPU_MS`（默认 30）的任务，把它的 sync fd 当释放栅栏，图像下次呈现的获取栅栏若比释放栅栏先信号就失败；`ZC_ACQUIRE_FENCE=1`/`2`（信号量加栅栏/只用栅栏）时每次等过获取栅栏都检查 `vkGetFenceStatus` 为 `VK_SUCCESS`、再等一次不超时 |
| `patches/winlator-0013-*` | 驱动有 `VK_KHR_external_semaphore_fd` 时服务端在设备上启用它，供零拷贝交换链把合成器的释放栅栏导入获取信号量；`vkWaitForFences` 导出未信号栅栏的 sync fd 后（导出有重置的副作用，临时导入的载荷会退回永久载荷）把 fd 的副本临时导回同一个栅栏，否则等过的获取栅栏仍读作 `VK_NOT_READY`，再等一次会永远挂住 |
| `patches/winlator-0015-*` | 客户端断开时释放它没销毁的一切（游戏和 DXVK 通常直接退出；以前服务端全留着，几次启动后设备上服务端占 1.07 GB 映射的 dma-buf 加 1.79 GB swap，内存耗尽）。每个连接（`VkContext`）记录自己建的 instance、device，以及每个 device 上的内存（`ResourceMemory`：fd、dma-buf、AHardwareBuffer）、交换链、着色器模块和全部驱动子对象（Mali 的 `vkDestroyDevice` 不回收没销毁的子对象：栅栏留着导入的 sync file，池和管线留着 GPU 映射），正常销毁时移出；客户端每个进程只有一条连接，id 只在返回它的连接上有效，所以按连接归属；销毁一个不属于本连接的对象（重复销毁、别的设备或连接的 id）只打日志（`untracked`）不执行。断开时：请求线程先处理完环里剩下的请求（干净退出时最后那批销毁以前会被丢掉）再结束，然后 status 置负：timeline 等待（`vkWaitSemaphores`）改为每 100 ms 一片，看到连接关闭就返回（以前无限等待的线程让线程池和整个清理永远卡住，连接关不掉），队列里没开始的异步管线创建跳过；`XWindowSwapchain_forgetContext` 之后，每个 device 在每个队列上提交只带栅栏的空批次、最多等 3 s；等不到时把它的每个 timeline 信号量从主机端 signal 到当前值加 `maxTimelineSemaphoreValueDifference`（客户端留下的等待它自己才会 signal 的值的工作就能跑完），再等 1 s；仍然忙的 device 交给一个后台线程，每 1 s（一分钟后每 10 s）查一次那些栅栏，空闲或丢失后照样释放、再销毁 instance 和 context（日志 `client gone, busy devices drained after X s`），然后按依赖顺序销毁剩下的对象（交换链、framebuffer、视图、管线、……、buffer、image、内存）、纹理解码器、管线库引用，再 `vkDestroyDevice`，最后 `vkDestroyInstance`；每次断开打一行 `vortek-server: client gone (fd=N): released 1 devices, 73 memory allocations (1057.6 MiB), 1 swapchains, 93 other objects, 1 instances`。顺带修了每次 `vkWaitSemaphores` 漏掉的请求副本、连接结束时没释放的 ShaderInspector 和额外数据。测试 `tests/leak_test.c`（`exit`：分配后不释放就 `exit()`；`kill`：等 SIGKILL；`clean`：全部按序销毁）和 `tests/leak_test.sh`（对同一个服务端启动 N 次，每次记录服务端的 VmRSS/RssAnon/RssFile/VmSwap、fd 数、线程数和持有的 dma-buf；`LEAK_MOCK=zc_mock_compositor` 时走零拷贝呈现；`LEAK_GPU_STUCK=1` 留下一个等永远没人 signal 的 timeline 值的批次，`LEAK_GPU_BUSY_MS=N` 让队列再忙约 N ms，runner 会等后台释放完成再采样）。64×16 MiB + 8 张 1024² 图像 + 交换链：修复前每次运行服务端多留约 1060 MiB dma-buf（76 个）、约 90 个 fd、4 个线程且连接关不掉，修复后 20 次 VmRSS 稳定在约 59 MB、fd 7、dma-buf 0 |
| `patches/vortek-0012-*`、`patches/winlator-0017-*` | 环形缓冲区的等待者计数不再被清掉：服务端建好两个环、发出 fd 后马上启动请求线程，客户端以前收到 fd 后又对共享头部做一次 `memset`；请求线程若已自旋完、把 `readWaiters` 记成 1 并睡下，这个计数就被清成 0，超时醒来减 1 后变成 0xFFFFFFFF，此后整条连接上它每次睡着计数都是 0，客户端从不唤醒它，每个请求都等满 50 ms 的 futex 超时（游戏 0.1–0.3 fps、CPU/GPU 空闲，或启动几分钟没有窗口，约每 5–10 次启动一次）。客户端改为 `RingBuffer_attach`（只映射，不清零、不重设 status）；两端的等待者递减都不会低于 0，所以即使计数被清过也只损失一次超时，服务端这时打一行 `ring: waiter count was cleared under a sleeping waiter (old client), repaired`。线上布局和协议不变，新旧客户端、服务端可以混用 |
| `patches/winlator-0018-*`、`src/cmd_memory.h` | Mali 驱动的命令缓冲内存泄漏：命令池的内存只要自上次释放以来写过一次查询（`vkCmdWriteTimestamp`/`vkCmdWriteTimestamp2`/`vkCmdBeginQuery`，一级或二级命令缓冲、在渲染内外、几个、哪种都一样），驱动就为以后录进这些内存的每个 `vkCmdBeginRendering` 留下 410–420 字节主机内存（与其中的绘制数无关），只有带 `RELEASE_RESOURCES` 的池重置、`vkTrimCommandPool` 或销毁池才归还并清掉这个标记（只单独重置命令缓冲的客户端：带标志的 `vkResetCommandBuffer`），flags 为 0 的重置和 `vkBeginCommandBuffer` 的隐式重置永不归还（不经 Vortek 直接在设备上跑也一样）。标记跟着池的内存走，不跟着录制：设备上的 bionic 探针里，同一个命令缓冲只在第一帧写一次时间戳，以后每帧照样每次 `vkCmdBeginRendering` 留 410 字节；内存还会在池里的命令缓冲之间换手，两个命令缓冲按和分配相反的顺序 begin 时，从没写过查询的那个也每次留 420 字节（顺序和分配一致时为 0）；池释放一次以后不再写查询就不再留。不留的：池里从没写过查询（只有 `vkCmdResetQueryPool` 或 `vkCmdCopyQueryPoolResults` 也不算）、`VkRenderPass` 的 `vkCmdBeginRenderPass(2)`；拷贝、清除、dispatch、屏障加查询但没有动态渲染也不留。DXVK 用 flags 0 重置命令池，游戏的 D3D11 查询和渲染录在同一个命令缓冲里，所以鸭科夫的服务端 Native Heap 每分钟涨约 85 MB，约 9.4 万帧后 Scudo 映射失败（`internal map failure`，栈在 `vkBeginCommandBuffer` 里 Mali 的 malloc）。服务端默认每个池（命令缓冲）每第 32 次重置释放一次（`VORTEK_CMD_RELEASE_EVERY=32` 的行为；设备上 30 分钟内存门槛里按估计释放时服务端 Scudo secondary 的 72 KB 块映射每分钟稳定涨约 21 个、Native Heap 每分钟涨 1.5 MB，每 32 次是有涨有落的锯齿，所以 Vortek 退回每 32 次；进程内的 `../hybris` 仍默认按估计）。按估计释放要选：`VORTEK_CMD_RELEASE_MODE=adaptive`（也认 `estimate`、`auto`，不分大小写），或设了 `VORTEK_CMD_RELEASE_BYTES`/`VORTEK_CMD_RELEASE_TOTAL_BYTES`，或 `VORTEK_CMD_RELEASE_EVERY=auto`；`VORTEK_CMD_RELEASE_MODE=every`（或 `fixed`）强制固定间隔（`_EVERY` 是数字就用它，否则 32）；MODE=adaptive 时 `_EVERY` 的数字忽略并打日志，不认识的 MODE 值忽略并打日志（按其余变量和默认）；没有 MODE 时 `_EVERY` 是数字优先于字节限额（规则在 `cmdmem_release_policy`，`tests/cmd_memory_test.c` 覆盖）。按估计释放（`src/cmd_memory.h`，和 `../hybris` 共用）：重放命令批时数 `vkCmdBeginRendering` 和查询写入，池里任何命令缓冲写过查询后这个池就带标记（池带标志重置时清掉），`vkEndCommandBuffer` 时池带标记（或这次录制自己写了查询）就给该命令缓冲和它的池记 420 字节 × 次数（以前只算同一次录制里既有查询又有渲染的，漏了 DXVK HUD frametimes 写在 init 命令缓冲里的时间戳：设备上 30 分钟每分钟涨 12.9 MB）；池的估计到 `VORTEK_CMD_RELEASE_BYTES`（默认 `16m`，可写 `512k`、`1g`、字节数）或这个连接所有池合计到 `VORTEK_CMD_RELEASE_TOTAL_BYTES`（默认 2 倍、且这个池或命令缓冲自己至少有两个限额中较小者的 1/16，免得闲置的池（DXVK 回收的命令列表）让活跃的池每次重置都释放）时，下一次 `vkResetCommandPool`/`vkResetCommandBuffer` 加 `RELEASE_RESOURCES`；flags 0 的池重置之后这些内存归池所有，单个命令缓冲带标志重置或释放都不还（设备上的 bionic 探针），所以不再记在命令缓冲名下；允许单独重置的池里该释放的命令缓冲在 `vkBeginCommandBuffer` 前先带标志重置（只在真正的隐式重置时：客户端已经用 flags 0 显式重置过的命令缓冲内存已归池，这时再带标志重置什么也不还）；没写过查询的池永不释放；带标记的池（命令缓冲）至少每 4096 次重置释放一次，兜底估计不知道的泄漏；`VkRenderPass` 加查询的池按兜底每 1024 次重置释放一次；表满了退回每 32 次。`VORTEK_CMD_RELEASE_LOG=1` 每次释放打一行（默认前 4 次和每第 64 次）。固定间隔（默认，或 `VORTEK_CMD_RELEASE_EVERY` 设成数字）每第 N 次重置释放（1 = 每次，0 = 关闭；每连接 64 项的表，新句柄或冲突时也释放），不做统计（`_EVERY` 既不是数字也不是 `auto`/`adaptive`/`estimate`/`every`/`fixed` 时忽略并打日志），固定间隔只包装两个重置函数，不管 `vkBeginCommandBuffer` 的隐式重置（本地 `FL_STEPS=timeline,submit2,draw,query,present` 默认下照样每帧涨 6.7 KB；DXVK 用池重置，不受影响，只靠隐式重置的客户端要用 `VORTEK_CMD_RELEASE_MODE=adaptive`）；服务端启动时打一行 `vortek: VORTEK_CMD_RELEASE_EVERY=32 (default; ...)` 或 `vortek: command memory released by estimate: ...`，供设备 A/B 选 rel0/rel32/按估计。本地（PalmCode proot，设备同时在跑游戏，噪声大）：类 DXVK 帧（16 个渲染通道 + 时间戳）按估计每千帧释放 0.33–0.4 次，每 32 次是 31.25 次；堆封顶在基线 + 2 × 16 MiB；不带释放的重置约 10–15 µs，带释放 30–70 µs（留存几 KB）、约 70 µs（200 KB）、约 240 µs（8 MB）、约 540 µs（17 MB）；释放后的下一次录制多 30–130 µs（小）到 0.5–0.7 ms（8–17 MB），所以每帧摊下来：每次释放 55–190 µs/帧，每 32 次 4–6 µs/帧，16 MiB 约 0.5 µs/帧，代价是一次 1–1.4 ms 的尖峰。顺带修了 `vkWaitSemaphores` 和异步管线创建每次漏掉的 32 字节（无底层缓冲的 MemoryPool 在 `vt_free` 后没释放分配列表的数组）。测试 `tests/frame_leak_test.c`（类似 DXVK 的帧：timeline、`vkQueueSubmit2`、动态渲染、描述符、查询、呈现，`FL_STEPS`/`FL_PASSES`/`FL_QUERY_*`（含 `reset`）/`FL_RENDERPASS=1/2` 等开关用来二分，`FL_QUERY_EVERY=N` 只在每个池每第 N 次使用时写查询（0 = 只有第一次；旧估计在这种帧下和 rel0 一样每帧涨 6.7 KB）；`FL_POOL_RELEASE=N` 由测试自己每第 N 次重置释放、`FL_TIMING=1` 打出带/不带释放的重置和随后录制的耗时；用 NDK 的 clang（`--target=aarch64-linux-android30`）编成 bionic 程序时不经 Vortek 直接跑设备驱动并打印本进程的堆）、`tests/frame_leak_test.sh`（默认 16 个渲染通道加一个时间戳，报告服务端堆的首次值、峰值、稳态、每千帧释放次数和服务端每帧 CPU；`FL_EXPECT=bounded`（默认：后一半的峰值比前一半每帧涨不超过 `FL_MAX_GROWTH` 64 字节）/`flat`/`grow`，见 `tests/frame_leak_report.awk`）和 `tests/cmd_memory_test.c`（估计逻辑的单元测试，不用 GPU）。设备上：修复前 6778 字节/帧，每 32 次 0；`VORTEK_CMD_RELEASE_EVERY=0` 时 6729 字节/帧 |
| `patches/winlator-0022-*` | 按估计释放的诊断：`VORTEK_CMD_RELEASE_STATS=<秒>`（或 `VORTEK_CMD_RELEASE_LOG=2`，每 30 秒），只在按估计释放时（`VORTEK_CMD_RELEASE_MODE=adaptive`）有效。每个周期每个连接打一行汇总（池数、带标记的池、估计总量、累计释放、查不到的命令缓冲/命令池次数），每个这期间用过、释放过或还带估计的池一行（池重置、单个命令缓冲重置、begin 和其中的隐式重置、end、看到的和计入估计的 `vkCmdBeginRendering`、带查询的录制、释放次数和量、估计、活着的命令缓冲数），再打一行进程的堆：Scudo 已分配字节和 `scudo:secondary` 映射数（Mali 留下的命令内存在设备上表现为 72 KB 的块）。用来在真实游戏上找估计漏算的部分；默认关闭，开销是每次重置/End 一次 `clock_gettime`。`tests/frame_leak_test.c` 新增 `FL_SECONDARY=1`：照 DXVK 3.x 在瓦片 GPU（所有 Mali）上的做法，每个渲染通道录进一个二级命令缓冲（`RENDER_PASS_CONTINUE` + 继承渲染信息，绘制和 occl/inrp 查询都在里面），主命令缓冲 `vkCmdBeginRendering`（`CONTENTS_SECONDARY_COMMAND_BUFFERS`）+ `vkCmdExecuteCommands`；`tests/frame_leak_test.sh` 的 `FL_SERVER_LOG=<文件>` 保留服务端日志，`VORTEK_CMD_RELEASE_LOG` 可覆盖（默认 1） |
| `patches/winlator-0019-*` | Winlator 的 `removeImageBoundCheck`（Mali + 引擎名 DXVK 时，把采样之后第一个第二操作数为零常量的 `OpSelect` 改成恒取第一个操作数）默认关闭：它把普通的 movc（`cond ? texel : 0`）也改成恒取纹理，而它要绕开的问题（spec 常量组合被折叠）已由客户端的 `src/spirv_spec_composite.h` 准确修好，且后者也覆盖计算着色器和内联阶段。`VORTEK_IMAGE_BOUND_CHECK_HACK=1` 恢复。测试 `tests/mali_quirks_test.c` 的 `bound`（运行时条件为假的那个输出必须是 0）；客户端和服务端要一起更新（APK 里本来就一起） |
| `src/pipeline_cache.c/.h`、`patches/winlator-0020-*` | 服务端的持久隐式管线缓存（同一份代码也编进进程内的 libhybris ICD，见 `../hybris/src/pipeline_cache_hybris.c`；用哪个缓存由 `PipelineCache_forGraphics/forCompute` 决定）。DXVK 不传 VkPipelineCache，Mali 驱动自己也不留着色器缓存（同进程换设备、换进程都是从头编译；驱动里只有厂商模糊效果的预编译文件，安卓的 blob 缓存只管 EGL，服务端又是绕过 libvulkan 直接加载 HAL 的独立进程），所以每次开游戏都把全部管线重新编译一遍：材质规模的着色器在 Mali-G2 上每个约 15 ms（GPL 的着色器库、后台的优化管线各一次），从缓存取约 0.2 ms。现在每个 VkDevice 带一个 VkPipelineCache：应用没传缓存时用于库、完整管线和计算管线（只做快速链接的 GPL 管线不用；带 shader module identifier 的请求也不用：identifier 是驱动按应用原始 SPIR-V 算的，客户端可能改写过，按它命中可能拿到另一种改写编出的旧管线），设备第一次用它建管线时从 `$VORTEK_PIPELINE_CACHE_DIR/<应用名>.<驱动键>.vkpc` 读入（应用名取 VkApplicationInfo 的 pApplicationName，DXVK/vkd3d 是 exe 名、zink 是进程名，进程环境里有 Steam 游戏 ID 时后面加 `-<ID>`（只有进程内路径看得到）；驱动键是 vendor/device ID、driverVersion、pipelineCacheUUID 和驱动信息串（Mali 的构建号）的哈希；文件头带同样的身份和数据哈希，数据开头还要是本驱动的 VkPipelineCacheHeaderVersionOne，不符就丢弃重建；驱动解析文件期间存在 `<文件>.loading`，由解析的进程 flock 着、放手前删掉：没人持有的标记是上次死在解析里的进程留下的；标记里记着连续死在解析里的次数，第一次再读一遍（进程也会因为别的原因死在解析中，比如会话结束被杀），连续第二次就删掉这份文件而不是再读，免得这个游戏每次启动都把服务端打崩；别的活进程正持有时等它（最多 10 秒，超时这次不读）；解析锁限时等待，`exit()` 最多等别的线程的解析 2 秒）。保存：有新管线且停止编译 2 秒后由后台线程写（至少隔 10 秒，最迟 `VORTEK_PIPELINE_CACHE_SAVE_S`，默认 30 秒；会话结束时 App 用 SIGKILL 杀服务端，没有退出钩子），设备销毁时（包括客户端被杀后的清理）再写一次；先写临时文件再 rename；同一目录一次只保存一个（进程内互斥锁，进程之间 flock 目录）；同一游戏两个设备同时写时合并成并集，并集没有比磁盘上多出东西就不写；内容没变（大小相同）不重写，先只查大小，不把整份缓存拷出来。上限 `VORTEK_PIPELINE_CACHE_MAX_MB`（单个文件，默认 64，超过就保留原文件）、`VORTEK_PIPELINE_CACHE_TOTAL_MB`（全部，默认 256，按最近使用淘汰）；`VORTEK_PIPELINE_CACHE=0` 关闭。App 把目录设成 `cacheDir/vortek-pipelines`（droiddeck-env 可改；不存在时连上级目录一起建）。每个设备结束时打一行 `vortek: pipeline cache <应用>: N pipelines in X ms (... by time: <2 ms ...)`，也就是每次会话编译了多少管线。测试 `tests/pcache_test.c`（按 DXVK 的 GPL 用法建 N 组管线：顶点库、片元库、快速链接、完整管线，每条管线画一个像素校验颜色，`PC_THREADS` 多线程）和 `tests/pcache_test.sh`（冷启动/被杀的客户端、新服务端进程读文件、服务端被 SIGKILL 前后台已保存、两个设备合并、损坏文件、大小上限和淘汰、关闭、identifier 请求不命中缓存（`PC_IDENT`）、残留 `.loading` 标记（一次再读、连续两次删）、只读缓存的设备合并时不重写、应用自带的超过 256 KB 的缓存经回复环保存和读回）。本机 60 组：冷 完整管线中位 16 ms、库 15 ms，热 0.5 ms（含 Vortek 往返；服务端 182 条共 40 ms），4 线程冷编译有无缓存一样快 |
| `src/client_x11_present.c/.h` | 客户端的软件 X11 呈现（MIT-SHM / PutImage） |
| `src/droiddeck_defaults.h` | 客户端默认 socket 路径 `/run/droiddeck/vortek/V0` |
| `src/spirv_clip_cull.h` | 去掉着色器里 ClipDistance/CullDistance 写入（Mali 编译器遇到会崩），客户端（`droiddeck_maint.c`）和进程内路径（`../hybris`）共用 |
| `src/spirv_spec_composite.h` | 由 spec 常量组成的 `OpConstantComposite` 改成 `OpSpecConstantComposite`（同长度、原地改；由它组成的组合常量一并改）。Mali（r56p0 也是）把这种组合按 spec 常量的默认值折叠，管线给的特化值对它无效；DXVK 的绑定检查（`OpSelect` 的条件是 spec bool 组成的 bvec4）就是这种形状，纹理读出默认值。客户端（`droiddeck_maint.c`）对着色器模块和管线阶段内联的 `VkShaderModuleCreateInfo`（图形和计算）都做，和进程内路径（`../hybris`）共用；`VORTEK_SPEC_COMPOSITE_FIX=0` 关闭。测试 `tests/mali_quirks_test.c`（`MQ_PARTS=spec,bound`；编成 bionic 程序直接跑驱动，`MQ_FIX=1` 时测试自己做这个改写） |
| `src/stage_inline_module.h` | 在管线阶段的 pNext 链里任何位置找内联的 `VkShaderModuleCreateInfo`，并做一条把它换成改写后代码的链（排在它前面的已知阶段结构复制一份；有不认识的结构就不改）。以前只看链上第一个结构，模块排第二时（例如前面是 `VkPipelineRobustnessCreateInfoEXT`）ClipDistance 去除和 spec 组合修复都不做，进程内 Mali 编译器直接段错误。客户端（`droiddeck_maint.c`）和进程内路径共用。`tests/mali_quirks_test.c` 的 `MQ_INLINE=2` 测这种链；经 Vortek 时这种链本来就传不过去（建管线返回 -4，改动前也一样），所以只在进程内路径上测 |
| `src/linear_row_pitch.h` | Mali（r56p0）对只有一行的 LINEAR 子资源（1D、高 1 的 2D 及 mip、高 1 的 3D）在 `vkGetImageSubresourceLayout` 里报 `rowPitch` 0，其余字段对；这时取 depthPitch、arrayPitch、size 里最小的非零值（就是对齐后的一行，16x1 RGBA8 为 64，300x1 R8 为 320，和 300x2 一样）。DXVK 把它当 D3D11 映射的 RowPitch 交给游戏，0 会让按行拷贝丢数据。客户端在 `vkGetImageSubresourceLayout`、`...2`/`2KHR`/`2EXT` 和 `vkGetDeviceImageSubresourceLayout` 上修，和进程内路径共用。测试 `tests/mali_quirks_test.c`（`MQ_PARTS=linear`，`MQ_INLINE=1` 时也查 `...2KHR`） |
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
- `droiddeck-game-env` 的 A/B 开关（默认不设，行为不变；可写进 droiddeck-env 或某个游戏的环境）：
  `DROIDDECK_DXVK_DESCRIPTOR_BUFFER=1/0` 往 `DXVK_CONFIG` 加 `dxvk.enableDescriptorBuffer = True/False`
  （DXVK 默认不对 Mali 打开；Vortek 不转发 VK_EXT_descriptor_buffer，所以只在 libhybris 路径上起作用）；
  `DROIDDECK_DXVK_UPDATE_TEMPLATES=1/0/32` 加 `dxvk.enableDescriptorUpdateTemplates`，`32` 只对 32 位游戏 exe。
  `WINE_OFFSCREEN_CHECK_MS=100` 让带 winex11 补丁（droiddeck-esync 包 rev 3 起）的 Proton 在呈现路径上
  复用窗口离屏检查，每帧少几次 wineserver 往返。

## 限制

- 呈现是软件路径（GPU → 共享缓冲区 → MIT-SHM → Xwayland → wl_shm → 合成器），多次拷贝，没有垂直同步。
- 没有 gamescope：没有 Steam 游戏模式的合成、缩放和性能浮层。
- Vortek 客户端只支持 X11（xlib/xcb）surface，不支持 Wayland surface。
- Steam Linux Runtime 容器（pressure-vessel）里能不能看到 `/run/droiddeck/vortek/V0` 和这个 ICD 还没验证。

## 许可

Vortek 客户端和 vortekrenderer 由 brunodev85 以 LGPL-2.1 发布。`patches/` 是对上游的修改，`src/` 里的新文件
同样以 LGPL-2.1（或更新版本）发布，文件头保留了相应声明。全文见 `LICENSE.LGPL-2.1`，来源说明见 `NOTICE`。
