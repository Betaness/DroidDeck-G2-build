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
| `patches/libhybris-0003-tls-thunk-fast-path.patch` | 驱动里每条 `mrs xN, tpidr_el0`（`libGLES_mali.so` 有 80861 处，96.5% 是读栈保护值）跳到的 TLS 跳板：原来每次都把 x16/x17/x30 存到栈上再检查本线程的 bionic TLS 有没有建好（2 次写、3 次读）；改为在目标寄存器里检查（mrs、add、add、ldr、cbz，再算一次地址返回：1 次读，不碰栈，不改标志位），只有线程第一次进驱动时才保存寄存器、调用初始化。跳板大小（80 字节）和初始化函数不变。PalmCode 的 Ubuntu 里绑大核交替测：`vkCmdBindDescriptorSets` 10.45→9.61 ns/条，fill+barrier 录制 55.0→53.0 ns/条 |
| `patches/libhybris-0004-sync-read-without-guard.patch` | 驱动调用的每个 bionic `pthread_mutex_*`/`pthread_cond_*`/`pthread_rwlock_*` 都经 libhybris 的钩子先读出对象里存的 glibc 对象指针，原来每次都在一把进程级全局锁（`static_sync_guard`）下读，所有线程抢同一把锁。对象只有 4 字节对齐，不能用 8 字节原子操作；改为发布时分两次 4 字节原子写（先低位，再 release 写高位），读时先 acquire 读高位，非零就是 init 或发布时存进去的值（宿主指针，或者高位全 1 的进程共享句柄，`HYBRIS_SHM_MASK`），不用拿锁，整个 64 位值原样返回，和加锁读到的一样，句柄仍由调用方照旧转换；高位为零（静态初始化值）照旧走锁。全局锁获取次数：thread_tls_test 约 2.4 万→0，事件/栅栏循环 1182 万→0；单线程 fill+barrier 录制 53.0→51.3 ns/条，多线程的收益要在 DroidDeck 的 proot 里量 |
| `src/icd.c`, `src/icd.h` | guest 加载器看到的 ICD。dlopen 同目录 `lib/libhybris-vulkan-icd.so.0`（libhybris 的 glibc ICD：实例由安卓的 libvulkan 创建，设备和命令直接进 HAL），只拦截需要的入口，其余原样交给它；不包装句柄。有交换链的设备在图形队列族里多要一个队列给 acquire 用，没有空余时用应用的队列并加锁 |
| `src/wsi.c` | X11 WSI：`VK_KHR_surface`、`VK_KHR_xcb_surface`、`VK_KHR_xlib_surface`、`VK_KHR_get_surface_capabilities2`、`VK_KHR_swapchain`。交换链图像是 AHardwareBuffer（`VK_ANDROID_external_memory_android_hardware_buffer` 导入），经 `$VORTEK_PRESENT_SOCKET` 用 Vortek 的零拷贝协议（`../vortek/src/zero_copy.c`，原样编译）交给合成器显示；acquire 把合成器的释放栅栏导入应用的信号量/栅栏（`headless_swapchain.c` 的做法）。是否能走合成器图层由 X 窗口树决定（`client_x11_present.c` 的逻辑：被别的窗口挡住、被父窗口裁剪、带 alpha 时拷贝进窗口，完全被挡时按 10 fps 节流），两份代码合在一个进程里。表面的 `minImageExtent` 是窗口大小（以前 1x1，Proton 全屏补丁因此建 1x1 宿主交换链、每帧 SUBOPTIMAL、每帧重建，见 `../vortek/README.md`）；重建循环保护 `../vortek/src/swapchain_loop.h` 两边共用 |
| `src/mali_fixes.c` | Vortek 客户端里属于驱动问题的补丁：去掉 ClipDistance/CullDistance 写入（`../vortek/src/spirv_clip_cull.h`，和 Vortek 共用；不去掉 Mali 编译器在本进程里段错误，见测试）；由 spec 常量组成的 `OpConstantComposite` 改成 `OpSpecConstantComposite`（`../vortek/src/spirv_spec_composite.h`，和 Vortek 共用：Mali 按默认值折叠这种组合，DXVK 的纹理越界检查读出 0），着色器模块、图形、计算和光追管线的内联阶段都改（`VkShaderModuleCreateInfo` 在阶段 pNext 链的任何位置都找，前面的结构复制一份：`../vortek/src/stage_inline_module.h`），`vkGetShaderModuleCreateInfoIdentifierEXT` 也按改写后的代码算（和模块的标识符一致，DXVK 按它查管线），和 ClipDistance 开关无关；带延迟操作的光追管线被改写时同步完成，返回 `VK_OPERATION_NOT_DEFERRED_KHR`；`VK_KHR_pipeline_binary` 不报给应用（它的键和二进制按原始 SPIR-V 编）；`DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0`（没有时看 `VORTEK_SPEC_COMPOSITE_FIX`）关掉；一行高的 LINEAR 子资源 Mali 报 rowPitch 0，`vkGetImageSubresourceLayout`、`...2`/`2KHR`/`2EXT`、`vkGetDeviceImageSubresourceLayout(KHR)` 之后改成这一行的大小（`../vortek/src/linear_row_pitch.h`，和 Vortek 共用）；vkd3d-proton 的 storage texel buffer 对齐、`VK_EXT_vertex_attribute_divisor` 别名；驱动没有、DXVK 又要求的核心特性 fillModeNonSolid、shaderClipDistance、shaderCullDistance 照 Vortek 服务端的做法报成支持，建设备时再关掉（没有它们 DXVK 找不到设备：`Skipping: Device does not support required feature 'fillModeNonSolid'`）。maintenance5/6、扩展过滤表（`ext_commands.h`）都是 Vortek 协议的需要，原生驱动不用；GPL 驱动自己支持，但库的部分和链接调用里规范忽略、允许悬空的状态（顶点输入/片元输出部分和快速链接的 `pStages` 等）要先丢掉：调用里有这种部分时复制一份建管线信息、照 Vortek 客户端的规则清掉（`../vortek/src/gpl_library_state.h`，和 Vortek 共用），着色器改写、隐式管线缓存的选择、流输出统计和驱动都用这份副本，从不读应用的 `infos[i].pStages`（以前这几处都会顺着悬空指针读，驱动自己也会，测试 `tests/gpl_test.sh`）；只有完整管线的调用和以前一样原样传下去。另外照 Vortek 服务端（`winlator-0018`）按估计的驱动留存量释放命令内存：Mali 对录进「自上次释放以来写过查询的命令池内存」的每次 `vkCmdBeginRendering` 留约 420 字节主机堆（查询写在池里任何一个命令缓冲、哪怕只写过一次都算，见 `../vortek/README.md`），不带 `RELEASE_RESOURCES` 的重置（和 `vkBeginCommandBuffer` 的隐式重置）不还，DXVK 用 0 重置池，不处理游戏进程每分钟涨几十 MB 直到崩溃。默认包装 `vkCreateCommandPool`/`vkDestroyCommandPool`/`vkAllocateCommandBuffers`/`vkFreeCommandBuffers`、`vkBegin/EndCommandBuffer`、`vkCmdBeginRendering(KHR)`、`vkCmdBeginRenderPass(2)`、`vkCmdBeginQuery(IndexedEXT)`、`vkCmdWriteTimestamp(2)`，按池累计估计（`../vortek/src/cmd_memory.h`，无锁查表，只有建/删池和分配/释放命令缓冲加锁）：池的估计到 `DROIDDECK_HYBRIS_CMD_RELEASE_BYTES`（没有时取 `VORTEK_CMD_RELEASE_BYTES`，默认 `16m`）或所有池合计到 `*_CMD_RELEASE_TOTAL_BYTES`（默认 2 倍，且这个池自己至少有较小限额的 1/16）时，下一次重置加 `RELEASE_RESOURCES`；flags 0 的池重置后内存归池，单个命令缓冲的释放不再抵扣；允许单独重置的池在 `vkBeginCommandBuffer` 前先带标志重置；池里有命令缓冲写过查询后，池里所有命令缓冲的 `vkCmdBeginRendering` 都计入，直到池带标志重置；没写过查询的池永不释放；写过查询的池至少每 4096 次重置释放一次（兜底）；显式 `vkResetCommandBuffer` 之后的 `vkBeginCommandBuffer` 不再补释放（内存已归池，释放不掉）；`*_CMD_RELEASE_LOG=1` 每次释放打一行。`DROIDDECK_HYBRIS_CMD_RELEASE_EVERY`（或 `VORTEK_CMD_RELEASE_EVERY`）设成数字时照旧每第 N 次重置释放（1 每次，0 不释放；256 格的表加锁），这时只包装两个重置函数，给设备 A/B 选 rel0/rel32/按估计；`DROIDDECK_HYBRIS_CMD_RELEASE_MODE`（或 `VORTEK_CMD_RELEASE_MODE`）`=every` 强制固定间隔（没有数字时 32），`=adaptive` 强制按估计（规则和 Vortek 共用：`cmdmem_release_policy`）。这条路默认仍按估计（30 分钟内存门槛通过，每分钟 +0.10 MB）；Vortek 服务端的默认改回了每 32 次（`../vortek/README.md`）。D3D11 流输出的被动统计（`../vortek/src/xfb_stats.h`，和 Vortek 客户端共用）：数 Xfb 管线、几何着色器管线、`vkCmdBeginTransformFeedbackEXT` 和 `vkCmdDrawIndirectByteCountEXT`，为此多包装 `vkDestroyShaderModule` 和这两条命令，呈现时看一眼要不要写日志行 `hybris-icd: xfb stats: ...`；测试 `../vortek/tests/xfb_vk_test.c`（`tests/run.sh` 下跑） |
| `src/pipeline_cache_hybris.c` | Vortek 服务端的持久隐式管线缓存（`../vortek/src/pipeline_cache.c`，原样编进来）在游戏进程里：DXVK 不传 VkPipelineCache、Mali 自己不留缓存，每次开游戏每条管线都要重编（本机约 13–15 ms，从缓存约 0.15 ms）。文件格式、文件名（`<应用名>.<驱动键>.vkpc`，同一驱动下和服务端的文件通用；进程有 Steam 游戏 ID（`SteamGameId`，没有时 `SteamAppId`）时是 `<应用名>-<ID>`，免得不同游戏的同名 exe 共用一份文件）、`.loading` 崩溃标记（记连续死在解析里的次数：游戏进程也会因为别的原因死在解析中，第一次只再读一遍，连续第二次才删文件）、第一次建管线时才读、后台线程保存（停止编译 2 秒后，最迟 30 秒，游戏被杀前已保存）、合并时没有新东西不重写、带 shader module identifier 的请求不用缓存、单文件和总大小上限都和服务端一样；游戏的几个进程共用一个目录，所以标记由解析的进程 flock 着（活进程的标记等待，死进程的才算崩溃），保存在进程之间 flock 目录；解析锁限时等待（持有它的线程可能已被 Wine 杀掉），`exit()` 最多等别的线程的解析 2 秒、在解析线程自己身上不等（以前会让游戏退出时卡死）。驱动经设备表调用，变量是 `DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR`（没有就不用缓存）、`DROIDDECK_HYBRIS_PIPELINE_CACHE=0`、`_MAX_MB`、`_TOTAL_MB`、`_SAVE_S`、`_LOG`（后几个没设时看对应的 `VORTEK_PIPELINE_CACHE_*`，目录不看）。droiddeck-game-env 把目录设成 `$XDG_CACHE_HOME/droiddeck/hybris-pipelines`（默认 `~/.cache/...`）：游戏进程在 guest 里，服务端的目录在 App 的安卓缓存目录，guest 看不到，要绑进来得多占一个 proot 快速路径的绑定，安卓还可能清掉它；guest 的缓存目录跟 rootfs 一起留着（Mesa 的着色器缓存也在这里）；放在每个游戏的前缀里则每个目录只有一份文件，总大小上限和按最近使用淘汰就管不了全部游戏 |
| `src/pass_barrier.h` | `DROIDDECK_HYBRIS_PASS_BARRIER`（不设等于 `off`）。DXVK 在每个同步渲染通道结束时调一次 `vkCmdPipelineBarrier2`：一个访问掩码全为 0 的全局内存屏障（dst 在 Mali 上是 0x01001ff8）和每个附件一个布局不变的释放屏障（可采样的渲染目标 dst 是 0x1cf8），两者都让之后的顶点工作等之前的片元工作。设了这个变量时 `mali_fixes.c` 包装 `vkCmdPipelineBarrier2` 和 `...2KHR`（DXVK 只调前者；`vkCmdPipelineBarrier` 不包装，DXVK 不调它），并扫描交给驱动的着色器代码（建模块、图形管线的内联阶段、`vkGetShaderModuleCreateInfoIdentifierEXT`）作守卫；不设或 `off` 时什么都不包装、不扫描。`log`：只计数。`narrow`：全局屏障（没有 pNext、两个访问掩码为 0、dst 含 CS、VS、FS、TRANSFER、COLOR_OUT、EARLY、LATE）的 dst 去掉 VS、TCS、TES、GS 和 XFB（0x01001ff8 变成 0x1f80）；见过声明流输出的代码或流输出管线、或有设备开了 `vertexPipelineStoresAndAtomics` 时不改。`narrow-rt`：`narrow` 之外，附件释放屏障（没有 pNext、布局和队列族不变、源访问只有附件写、源阶段含颜色输出或深度测试、dst 含几何阶段）的 dst 去掉 VS、TCS、TES、GS（0x1cf8 变成 0x1c80）；见过顶点、细分或几何着色器读 Dim 不是 Buffer 的图像（采样、fetch、gather、`OpImageRead`、它们的稀疏形式、`OpImageTexelPointer`；跟不到类型的操作数和解析不了的代码按读算）的代码，或开了 `vertexPipelineStoresAndAtomics` 时，图像屏障不改。`nogeo`：源阶段含片元阶段的屏障（内存、buffer、图像）都去掉 VS、TCS、TES、GS 和 XFB，不设守卫，用来量上限，画面可能出错。只改 dst 阶段，访问掩码、布局、队列族、标志和 pNext 不动；去掉后 dst 为空、或剩下的阶段不再覆盖它的访问类型（着色器访问没有着色器阶段了、表里没有的访问类型）时保持原样，有流输出访问时留着 XFB 阶段。只复制有改动的数组：内存屏障 4 个、buffer 屏障 8 个、图像屏障 16 个以内放栈上，再多 malloc，分配失败就原样传下去。设了时每 `DROIDDECK_HYBRIS_PASS_BARRIER_LOG_S` 秒（默认 60，呈现时看）和销毁设备时写一行 `hybris-icd: pass barrier: ...`（进程开始以来的累计数：presents、calls、global（全局屏障形状）、global-writes（其中源访问不为 0 的，不收窄）、attachment（附件释放屏障形状）、frag-to-geo（源含片元阶段、dst 含几何阶段或 XFB 的屏障）、narrowed（改了的屏障）、global-dst 和 attachment-dst（最近一次见到的 dst）、geo-image-code（读图像的几何阶段代码交给驱动的次数，同一份代码可能算多次），和三个守卫的状态），没有新屏障时不写；守卫第一次触发时写一行 `hybris-icd: pass barrier guard: ...`。测试 `tests/pass_barrier_test.c`（规则）和 `tests/pass_barrier_icd_test.sh`（经加载器，看驱动收到的掩码），CI 的 hybris 任务里也跑 |
| `src/placed.c` | Mali 没有的 `VK_EXT_map_memory_placed`，给 WoW64 的 Wine（32 位游戏的映射要在 4 GB 以下）。Wine 在设备有它时用 `vkMapMemory2KHR` 把内存映射到自己预留的低地址，没有时退到 `VK_EXT_external_memory_host`：给每个 `vkCreateImage` 加 `HOST_ALLOCATION_BIT_EXT`（Mali 返回 -1000158000），主机可见内存导入低地址的匿名内存（Mali 只有缓存、非一致的类型能导入，0x7 类型找不到替代，驱动映射在 4 GB 以上，Wine 报 `VK_ERROR_OUT_OF_HOST_MEMORY`）。照 Vortek 客户端和服务端的做法：开了这个扩展的设备上，主机可见内存是 dma-buf，按应用要的类型导入，placed 映射就是把 dma-buf mmap 到调用者给的地址，解除映射时按 `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` 保留地址范围。GPU 不探查 CPU 缓存，所以 HOST_COHERENT 类型只用 system-uncached 堆（没有就分配失败，不退到有缓存的堆）；HOST_CACHED 而非 coherent 的类型（Mali 的 0xb）用有缓存的 system 堆，应用的 flush/invalidate 在用户态按范围做缓存维护（`dc cvac` / `dc civac`，不像 `DMA_BUF_IOCTL_SYNC` 那样总是同步整块），uncached 的只排空写缓冲（`dsb sy`）；这种内存的 flush/invalidate 不进驱动（驱动按自己的映射做，没有映射时空指针崩溃）。`DROIDDECK_HYBRIS_PLACED_CACHED=0` 时 0xb 类型也用 uncached 堆。专用于最优图像的分配（`VkMemoryDedicatedAllocateInfo` 带的图像不是 linear，靠拦截 `vkCreateImage`/`vkDestroyImage` 记下 linear 图像）直接交给驱动：Mali 的图像类型都是主机可见的，否则 32 位游戏的每张渲染目标都是一个 uncached dma-buf 和一个 fd；DXVK 不映射最优图像，vkd3d-proton 映射 CPU 可访问的纹理时用 linear，Vortek 客户端也这样做；代价是对这种内存的 placed 映射会失败（`VK_ERROR_MEMORY_MAP_FAILED`，日志里有一行说明），直接映射最优图像专用内存的 32 位原生 Vulkan 程序要设 `DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES=1`，这类分配也用 dma-buf。堆设备只打开一次（proot 下每次 open 都要经过跟踪器）；拿不到或导入不了 dma-buf 时分配直接返回 `VK_ERROR_OUT_OF_DEVICE_MEMORY`（DXVK 会换个办法再分配），不再悄悄退回驱动内存、等映射时才失败。别的内存、没开扩展的设备（64 位游戏）不经过这里。`DROIDDECK_HYBRIS_MAP_PLACED=0` 隐藏扩展 |
| `src/ahb.c`, `src/compat/android/hardware_buffer.h` | AHardwareBuffer 调用经 libhybris 转到设备的 `libnativewindow.so`（分配走 gralloc 的 binder 服务，和普通 App 一样） |
| `tests/` | 见下 |

环境变量沿用 Vortek 的同名开关：`VORTEK_CMD_RELEASE_MODE`/`_EVERY`/`_BYTES`/`_TOTAL_BYTES`/`_LOG`（`DROIDDECK_HYBRIS_CMD_RELEASE_*` 优先）、`VORTEK_PRESENT_SOCKET`、`VORTEK_ZERO_COPY=0`、`VORTEK_AHB_SWAPCHAIN=0`、`VORTEK_MIN_IMAGES`、
`VORTEK_RELEASE_WAIT`、`VORTEK_X11_SHM=0`、`VORTEK_X11_STATS=N`、`VORTEK_RECREATE_LIMIT`（重建循环保护，`../vortek/src/swapchain_loop.h`）、`VORTEK_KEEP_CLIP_DISTANCE=1`、`VORTEK_STRICT_TEXEL_ALIGNMENT=1`。
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
  `VORTEK_PRESENT_SOCKET=$XDG_RUNTIME_DIR/vortek-present`，`DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR=$XDG_CACHE_HOME/droiddeck/hybris-pipelines`
  （已设的不改），`GLIBC_TUNABLES` 加 `glibc.rtld.optional_static_tls=16384`
  （启动脚本的 `LD_AUDIT` 让 rtld 在预加载之前就排好静态 TLS，libhybris-common 约 4 KB 的 TLS 段只能从可选余量里分）。缺文件或 `/system/lib64/libvulkan.so` 时退回 Vortek。
- `SessionFiles` 每次会话从 APK 放进 rootfs（有才放）；CI 的 `hybris` 任务（`continue-on-error`）编出后放进 APK。
- 合成器（`waylandcomp/src/vortek_present.c`）现在同时接受最多 7 个零拷贝连接（Vortek 服务端 + 各游戏进程），
  链 id 按连接区分，释放消息发回所属连接。只有 Vortek 服务端一个连接时行为和以前一样：同一进程重连会替换旧连接，
  最后一个连接断开时收起所有 Vortek 图层；还有别的连接时，收起没有窗口在用的图层（包括记账丢失的）。

## 测试（PalmCode 的 Ubuntu 里能跑；需要能读到 /system、/vendor、/apex）

PalmCode 的 proot 拦截 ioctl（约 50 µs）、futex（约 45 µs）和 send/recv（约 34 µs），DroidDeck 自己的 proot 不拦（`docs/development/proot-performance.md`）。
这里只适合功能测试和纯 CPU 的录制基准；提交、等待、多线程争用的耗时要在 DroidDeck 的 proot 里量。

```sh
sh build.sh                        # 第一次要联网
sh tests/run.sh vulkaninfo --summary
sh tests/run.sh /path/to/compute_glibc        # 私有原型的计算测试，结果和 /opt/hybris 原型一致
sh tests/wsi_test.sh               # Xvfb 上：X11 拷贝（BGRA/RGBA）、零拷贝（mock 合成器验像素）、改窗口大小、
                                   # 部分/完全遮挡切换、vkcube 零拷贝（最后一帧在 out/tests/vkcube.ppm）、
                                   # ../vortek/tests/fshack_test.c：Proton 全屏补丁（宿主交换链取 minImageExtent，
                                   # 计算着色器写 STORAGE 交换链图像，零拷贝和拷贝）、窗口模式布局、重建循环保护
sh tests/compositor_test.sh        # 合成器自己的 vortek_present.c（SurfaceControl 打桩）同时服务 3 个进程，
                                   # 再用 presenter_probe 测同进程重连和放不进链 key 的 id
glslangValidator -V --vn clip_vert -o out/tests/clip_vert.h tests/clip.vert && cc -Iout/tests -o out/tests/clip_test tests/clip_test.c -lvulkan
sh tests/run.sh out/tests/clip_test           # 写 ClipDistance 的管线；VORTEK_KEEP_CLIP_DISTANCE=1 时驱动段错误；
                                   # 内联代码排在 VkPipelineRobustnessCreateInfoEXT 后面也要改写；建模块和按建模块信息算的标识符一致
cc -O2 -o out/tests/mali_quirks_test ../vortek/tests/mali_quirks_test.c -lvulkan
MQ_PARTS=spec,bound,linear sh tests/run.sh out/tests/mali_quirks_test   # MQ_INLINE=1、=2 各再跑一遍（内联阶段、计算管线；=2 时排在链里第二位）；
                                   # DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0 时 spec 3 项、bound 1 项失败（驱动的折叠）；
                                   # linear 11 项（MQ_INLINE=1 时 33 项，含 Layout2KHR 和 DeviceImage...KHR）
sh tests/gpl_test.sh               # ../vortek/tests/gpl_test.c 在本进程里：四种库部分各建两遍、快速链接和 LINK_TIME_OPTIMIZATION 链接、
                                   # 画三角形核对像素；建管线信息里规范忽略的状态带悬空指针（0x10）；默认、关掉着色器改写、再关掉隐式缓存三遍；
                                   # 修之前三遍都在建库时段错误。都设 GPL_KEEP_LIBS=1：链接后销毁库再用快速链接的管线，Mali 原生也崩
                                   # （Vortek 服务端替应用留着库，这条路没有；DXVK 自己留着）
cc -O2 -Wall -Wextra -Isrc -o out/tests/pass_barrier_test tests/pass_barrier_test.c && out/tests/pass_barrier_test
                                   # DROIDDECK_HYBRIS_PASS_BARRIER 的规则（src/pass_barrier.h，不用 GPU）：DXVK 的全局屏障和附件释放屏障在各模式下的 dst，
                                   # 各个守卫，不该改的屏障（访问掩码、布局、队列族、pNext、改完为空或不再覆盖访问类型）；pb_rewrite 不写应用的数组、
                                   # 只复制有改动的数组（多了用堆）、计数；几何阶段读图像的 SPIR-V 扫描：glslang 编的 7 个着色器
                                   # （tests/pass_barrier_gen.sh 生成 tests/pass_barrier_spv.h）和手写的代码（跟不到类型的操作数、损坏的代码、texel 指针）
sh tests/pass_barrier_icd_test.sh  # 同一开关经加载器和本 ICD：tests/pass_barrier_next_icd.c 当 DROIDDECK_HYBRIS_NEXT_ICD，打印驱动收到的 dst，
                                   # 后面默认接 lavapipe（mesa-vulkan-drivers，不用安卓）；设备上 PB_NEXT_ICD=<out/guest 里的 lib/libhybris-vulkan-icd.so.0>
                                   # 接 Mali。各模式下 DXVK 那次调用的掩码（读图像的顶点着色器模块建出前后各一次）、销毁设备时那行的 narrowed、守卫行；
                                   # vertexPipelineStoresAndAtomics 开着时不改；不认识的值当 off
cc -O2 -o out/tests/thread_tls_test tests/thread_tls_test.c -lvulkan -lpthread && sh tests/run.sh out/tests/thread_tls_test
                                   # 3 轮各 16 个新 glibc 线程直接进驱动（TLS 跳板第一次走初始化、之后走快路径），录制、提交、核对 GPU 写的数据；
                                   # TT_BENCH=1 另测主线程录制每条命令的耗时
sh tests/frame_leak_test.sh        # ../vortek/tests/frame_leak_test.c 在本进程里（离屏，每帧 16 个渲染过程 + 时间戳查询），
                                   # 量进程自己的堆（首次报告、峰值、稳态）、每千帧释放次数和进程 CPU：池重置、单缓冲重置、
                                   # vkBeginCommandBuffer 隐式重置三种用法；释放关掉时每帧约涨 6.7 KB；每 32 次时平；
                                   # 按估计（默认 16m）有界，每千帧释放 0.4 次（每 32 次是 31.25 次）；4 个池合计上限；
                                   # 无查询不释放；VkRenderPass + 查询走 1024 次重置的兜底；查询只在每个池第一帧写一次
                                   # （FL_QUERY_EVERY=0）时关掉释放照样每帧涨 6.7 KB，按估计有界
                                   # 什么都不设时按估计（这条路的默认）；*_CMD_RELEASE_MODE=every 时每 32 次、平
cc -O2 -I../vortek/src -o out/tests/cmd_memory_test ../vortek/tests/cmd_memory_test.c -lpthread && out/tests/cmd_memory_test
                                   # 估计逻辑和策略选择（MODE/EVERY/字节限额/默认）的单元测试（不用 GPU，含 8 线程并发）
cc -O2 -o out/tests/pcache_test ../vortek/tests/pcache_test.c -lvulkan -lpthread
glslangValidator -V --vn pcache_exit_comp -o out/tests/pcache_exit_comp.h tests/pcache_exit.comp
cc -Iout/tests -o out/tests/pcache_exit_test tests/pcache_exit_test.c -lvulkan -lpthread && sh tests/pcache_test.sh
                                   # 持久管线缓存（Vortek 的 pcache_test.sh 改成多进程）：冷/热、被 SIGKILL 前已保存、两个进程合并、
                                   # 损坏文件、大小上限和淘汰、三种关法、identifier 不命中、死进程留下的标记（一次再读、连续两次删）、活进程持有的标记
                                   # （等 3 秒后照常读；持有 14 秒时 10 秒后放弃但不删文件）、只读缓存的进程不重写、
                                   # 读缓存的线程被按 Wine 的方式杀掉后 exit() 不卡（pcache_exit_test）、Steam 游戏 ID 进文件名。
                                   # 本机 60 组：冷 完整管线中位 14.6 ms，热 0.19 ms；182 条 2570 ms → 34 ms
cc -o out/tests/wow64_memory_test tests/wow64_memory_test.c -lvulkan && sh tests/run.sh out/tests/wow64_memory_test
                                   # WoW64 winevulkan 对驱动的调用：设备有 placed 映射时把每种主机可见类型映射到 4 GB 以下，
                                   # GPU 拷贝和填充后核对两边的数据；DROIDDECK_HYBRIS_MAP_PLACED=0 时复现 -1000158000 和 4 GB 以上的映射
cc -O2 -o out/tests/placed_memory_test tests/placed_memory_test.c -lvulkan && sh tests/run.sh out/tests/placed_memory_test
                                   # placed.c 的内存：每种主机可见类型用哪个 dma-buf 堆（/proc/self/fdinfo），先读进缓存再只 flush/invalidate
                                   # 一段范围时 GPU 和 CPU 能否看到对方的数据，300 轮分配释放不漏 fd，没有 fd 时分配失败，最优图像专用内存的 placed 映射失败
                                   # （DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES=1 时成功）；PM_BENCH=1 另测带宽和同步耗时
```

## 只能在设备上测的

- DroidDeck 会话里（DroidDeck 自己的 proot、guest 是 Arch）游戏进程能否加载：Proton/Wine 进程里 `LD_PRELOAD` libhybris-common、
  wine 的地址空间预留和 bionic 链接器能否共存，WoW64 32 位游戏，FEX 在同一进程里。
- 真合成器的显示（SurfaceControl 图层、AFBC 缓冲、释放栅栏），和 Steam 客户端（Vortek）同时出图时的图层叠放和遮挡。
- 帧率和 CPU：和 Vortek 的 A/B。
- 驱动崩溃现在直接带走游戏进程（不再是整个 Vortek 服务端）。
- `DROIDDECK_HYBRIS_PASS_BARRIER`：r56p0 是否按收窄后的阶段掩码让前后两个渲染通道重叠，收窄后画面是否正确，帧率和每帧 GPU 周期有没有变化，
  游戏里的计数（每帧多少个全局屏障和附件释放屏障，守卫有没有触发）。

## 许可

`libdroiddeck_hybris_vk.so`：LGPL-2.1 或更高（含 Vortek 的代码）。libhybris：Apache-2.0，链接器（q.so）来自 bionic，BSD-2-Clause（部分 Apache-2.0）；
另有 strlcpy/strlcat（ISC）、integrity_sha256.c（MIT）、bionic_asm_arm64.h（BSD-3-Clause），声明全文在 `NOTICE`。
android-headers：Apache-2.0。运行时加载的安卓系统库和驱动来自设备本身，不随 APK 分发。详见 `NOTICE`。
