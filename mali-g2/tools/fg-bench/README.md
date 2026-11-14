# fg-bench：ZeroFG 插帧在 Mali 上的 GPU 开销

调研 RESEARCH-2026-11-11 第 9 项：在合成器里用 ZeroFG（只要两张颜色帧）插帧。ZeroFG 只有 Adreno 840 上的数字
（ReallyZero 0.45 / 0.94 / 1.62 ms，Zero 2.18 / 3.37 / 6.42 ms，640×448 / 1280×896 / 1920×1344），没有 Mali 的。
这个程序在 guest 里经进程内的 Mali 驱动（`mali-g2/hybris`，和合成器同一个 `vulkan.mali.so`）量每生成一帧的 GPU 时间。

## 量什么

- 每个尺寸、每种模式（ReallyZero、Zero）：在 GPU 上做出真帧（一张每 512 px 重复的纹理，每帧平移偶数个像素，左上角
  一块不动的「HUD」），放进三张图的环里；ZeroFG 用相邻两帧生成中间帧，写进输出图。相邻的对共用一帧（A→B、B→C…），
  序号递增，和合成器喂它的方式一样（`--fresh`：序号 0，每对从头算）。
- 每次迭代一个命令缓冲：真帧拷贝、ZeroFG 录的全部命令；三个 `ALL_COMMANDS` 时间戳（每个都等前面的工作做完才写）分出
  「真帧拷贝」和「ZeroFG 本身」。默认最多 3 次生成同时在飞（GPU 一直有活，频率不掉下去）；`--serial` 每次等完再录下一次，
  另给出提交到完成的墙钟时间。
- 输出：每个尺寸×模式一行 GPU ms/生成帧的中位数、p90（还有最小、最大、平均、第一次），`Resize`（建管线和工作资源）的
  CPU 时间，真帧拷贝的 GPU 中位数（等于「把上一帧复制一份留着」的开销：同尺寸、同格式的整帧 `vkCmdCopyImage`）。
- 检查：计时之后读回最后一对和它的输出。输出的校验和与 A、B 都不同、且不是单色，才算通过（`check ok`）。另外打印输出、
  A、B、五五混合各自和精确中间帧（纹理平移半步）的平均绝对误差（0..255，去掉运动带进来的边），输出和 A、B 的差；输出几乎
  就是 A 或 B（两个引擎遇到「场景切换」时整帧显示后一帧）时注明。
- 默认：1704×1136 和 3408×2272，两种模式，预热 10 次 + 计时 60 次，输入 BGRA8（游戏零拷贝的 AHB：hybris WSI 把 DXVK 的
  `B8G8R8A8` 交换链建成 `HAL_PIXEL_FORMAT_BGRA_8888`），输出 RGBA8（合成器图层池的格式 `VKU_FMT`）。非 A2B10G10R10 的输出
  ZeroFG 先写自己的内部图再 blit，所以输出 RGBA8 包含这次 blit。
- 运动：默认每帧约宽/128 × 高/256（取偶数），1704×1136 是 12,4，3408×2272 是 26,8；`--motion DX,DY` 指定。

`--json` 时每个结果另有一行 `FGBENCH_JSON {...}`（`gpu_median_ms`、`gpu_p90_ms`、`copy_median_ms`、`resize_ms`、
`mae_out`、`mae_blend`、`near_copy` 等）。退出码：0 正常，1 检查没过，2 没有可用设备或 ZeroFG 不支持，3 参数错，4 Vulkan 错误。

```
fg-bench [--size WxH]... [--mode zero|reallyzero|both] [--iters N] [--warmup N] [--serial]
         [--in bgra8|rgba8|a2b10] [--out rgba8|bgra8|a2b10] [--backend auto|compat|modern]
         [--motion DX,DY] [--fresh] [--no-fp16] [--device N] [--loader PATH] [--json] [--list]
```

和合成器里真正接上时不同的地方（这个程序量不到的）：输入是程序自己分配的 OPTIMAL 图，不是导入的 AHB；在 guest 进程里自己
的设备和队列上跑，没有全局优先级；同时在跑的只有会话里的其他东西（合成器、Steam 界面），不是游戏。

## 文件

| 文件 | 作用 |
|---|---|
| `fg_bench.cc` | 程序。Vulkan 加载器运行时 `dlopen`（`libvulkan.so.1`，或 `--loader`/`FGBENCH_LOADER`），不链接；开 Vulkan 1.3、`synchronization2`、`shaderStorageImageExtendedFormats`，有就开 `shaderFloat16`；用图形+计算队列族（输出要 blit） |
| `build.sh [out]` | 编出 `out/fg-bench`（aarch64 glibc，静态链 libstdc++/libgcc，只依赖 libc），检查需要的 glibc 不高于 `GLIBC_MAX`（默认 2.38；本机 Ubuntu 24.04 编出来要 2.38，guest 运行时是 2.43），并把 `fg-bench-guest.sh` 复制过去 |
| `fg-bench-guest.sh` | 在 guest 里跑（`/tmp/ab/cpu/g.sh` 或 `droiddeckctl guest`）：`install`（从 App 的 agent-inbox 拷到 `~/.cache/droiddeck-fgbench`）、`info`（hybris ICD、`/system/lib64/libvulkan.so`、加载器、glibc，再用 hybris ICD 跑 `--list`）、`run [参数]`（按 droiddeck-game-env 给 hybris 游戏的环境：`VK_ICD_FILENAMES`/`VK_DRIVER_FILES` 指向 hybris ICD，`libhybris-common.so.1` 放 `LD_PRELOAD` 最前，`GLIBC_TUNABLES` 加 `glibc.rtld.optional_static_tls=16384`；`GPU=vortek` 时不改环境，走 Vortek；不带参数时用默认值加 `--json`）、`clean` |
| `test-local.sh` | 本机离线检查：用 lavapipe（Mesa 的 CPU Vulkan，只看对不对，时间没有意义）跑小尺寸的各种组合，每次都要退出 0、`ok`、不是整帧拷贝、比五五混合更接近中间帧 |
| `tests/cpuinfo_shim.c` | 只给本机 lavapipe 用的预加载：把 `/proc/cpuinfo` 的 CPU part 都显示成 Cortex-A76。K70 的内核没给用户态开 SVE，LLVM 却按 CPU 型号生成 SVE 指令（`addvl`），llvmpipe 的着色器一跑就 SIGILL；Mesa 的着色器磁盘缓存也要关（`MESA_SHADER_CACHE_DISABLE=true`），否则会读回之前编出的 SVE 代码 |
| `third_party/zerofg/` | ZeroFG v1.1.0（2b7711e4）的引擎源码、头文件、预编译 SPIR-V 和 GLSL，原样复制，按 Apache-2.0 使用（`LICENSE`、`UPSTREAM.md`） |

ZeroFG 本身的要求（源码里的检查）：Vulkan 1.3 + `synchronization2` + `shaderStorageImageExtendedFormats`；输入格式要能线性
过滤采样；`A2B10G10R10` 要能当存储图、采样、blit 源；只支持 0.5 相位（每对一帧）；尺寸 64..16384，Zero 两边都要 ≥128
（它的稠密光流在半尺寸上算，半尺寸低于 64 时返回 invalid argument）。

## 本机怎么跑

```sh
sh mali-g2/tools/fg-bench/build.sh
sh mali-g2/tools/fg-bench/test-local.sh
# 单独跑（lavapipe，本机要带上垫片、关着色器缓存）：
cc -O2 -shared -fPIC -o /tmp/cpuinfo_shim.so mali-g2/tools/fg-bench/tests/cpuinfo_shim.c -ldl
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_SHADER_CACHE_DISABLE=true LD_PRELOAD=/tmp/cpuinfo_shim.so \
  mali-g2/tools/fg-bench/out/fg-bench --iters 3 --warmup 1
```

## 设备上怎么做（平板，G2 会话）

前提：G2 开着会话（Steam 界面起来即可，不开游戏，免得和游戏抢 GPU）；「设置 > 诊断 > 调试工具」开着（`droiddeckctl guest`
要它）；GPU 路径是 hybris（O3 默认就是；`info` 里 `/system/lib64/libvulkan.so` 和 hybris ICD 都在才行）。下面在本机这个
工作树的根目录执行；`/tmp/ab/bin/adb` 是测试工具的 adb 垫片，总是连到平板。整个 `run` 预计一两分钟以内（4 组各 70 次，
加上建管线），`T=300` 留了余量。

```sh
cd /root/work/wt-framegen
sh mali-g2/tools/fg-bench/build.sh
I=/sdcard/Android/data/com.droiddeck.launcher.g2/files/agent-inbox
/tmp/ab/bin/adb shell mkdir -p $I
/tmp/ab/bin/adb push mali-g2/tools/fg-bench/out/fg-bench mali-g2/tools/fg-bench/fg-bench-guest.sh $I/
/tmp/ab/cpu/g.sh mali-g2/tools/fg-bench/fg-bench-guest.sh install
/tmp/ab/cpu/g.sh mali-g2/tools/fg-bench/fg-bench-guest.sh info
T=300 /tmp/ab/cpu/g.sh mali-g2/tools/fg-bench/fg-bench-guest.sh run | tee /tmp/fgbench-$(date +%m%d-%H%M).txt
/tmp/ab/cpu/g.sh mali-g2/tools/fg-bench/fg-bench-guest.sh clean
```

- `g.sh` 把本地的 sh 脚本经 `droiddeckctl guest` 送进正在运行的会话的 guest 里执行（base64 进、跑完删），打印它的输出，
  退出码照传（99：会话没开或调试工具关着）。二进制文件不能这样送，所以先 `adb push` 到 App 的 agent-inbox（guest 里是
  `/storage/emulated/0/Android/data/com.droiddeck.launcher.g2/files/agent-inbox`），`install` 再拷到 guest 的
  `~/.cache/droiddeck-fgbench`（存储卡上不能执行）。
- `install` 之后在 guest 的任何 shell 里一行就能跑：`sh ~/.cache/droiddeck-fgbench/fg-bench-guest.sh run`（参数照传给
  fg-bench，比如 `run --mode reallyzero --size 3408x2272 --iters 30`）。
- 只想要一个数时缩短：`run --size 3408x2272 --iters 30 --warmup 5 --json`。对照 Vortek：`GENV=GPU=vortek T=300 /tmp/ab/cpu/g.sh ... run`。
- 结果看 `GPU ms/generated frame` 那行的 median 和 p90，以及 `check ok`；`NOTE: the output is frame B shown whole` 表示
  那一档引擎把这对当成了场景切换，时间不代表正常插帧。
