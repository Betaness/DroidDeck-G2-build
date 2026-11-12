# 游戏主线程的时间分布（FEX + Wine + DXVK，按模块和类别）

问题：鸭科夫这类主线程受限的游戏，主线程的 CPU 时间花在哪里：FEX 翻译出来的客体代码（按客体模块：UnityPlayer.dll、
mono-2.0-bdwgc.dll、Mono 的 JIT 代码……）、FEX 自己（分发器、编译器和查找慢路径）、Wine（unix 侧和 PE 侧）、DXVK、
驱动和 libhybris、内核。

不用 root，也不用重编 FEX：

- **采样**：系统自带的 `simpleperf` 从 adb 用 `--app` 启动时，`simpleperf_app_runner` 把它切到 G2 的 uid 和 SELinux
  域，再进 `simpleperf` 域（AOSP `system/sepolicy/private/simpleperf.te`：`perf_event { open read write kernel }`，
  可读 `untrusted_app_all` 的 `/proc`）。前提是包可以从 shell 剖析：G2 的清单里有 `<profileable android:shell="true"/>`
  （本仓库 eeb94e2c3 起；上游没有）。proot 下的游戏进程和 App 同 uid、同 SELinux 域，`perf_event_open` 只做
  `ptrace_may_access(READ)` 检查，不需要 attach，所以被 proot 跟踪着也能采。内核有 perf LSM 钩子时
  `perf_event_paranoid` 是 -1，由 SELinux 管（AOSP `rootdir/init.rc`）；老内核要先 `setprop security.perf_harden 0`。
- **JIT 代码归属**：FEX 的代码缓冲区是匿名内存。不开命名时它叫 `[anon:FEXMemJIT]`，分发器在 `[anon:FEXMem_Misc]`
  （FEX eff9f459d `SharedCodeBufferManager.cpp`、`Dispatcher.cpp`），所以光看映射名就能分出「翻译出的代码 / 分发器 /
  libarm64ecfex.dll」。要按客体模块分，开 `FEX_BLOCKJITNAMING=1`：FEX 每编译一个块写一行
  `<宿主地址> <大小> <模块路径>+0x<偏移> (<宿主地址>)`，不在任何 PE 映像里的块（Mono 的 JIT 输出等）写
  `JIT_0x<客体地址>_<宿主地址>`（`Core.cpp:945-967`）。
  - Windows 版 FEX 用 C 运行库 `open("/tmp/perf-<unix pid>.map")`（`JitSymbols.cpp`），Wine 按进程当前盘符解析：
    Z: 是 guest 的 `/tmp`，C: 是前缀的 `drive_c/tmp`。文本模式写，行尾是 CRLF。`guestsnap.py` 把前缀的每个盘都找一遍。
  - 从 FEX 磁盘缓存载入的块没有符号（`Core.cpp` 在缓存命中时直接返回），所以命名的那一场同时设 `FEX_DISKCACHE=0`
    （`Core.cpp:892-919`；Proton 用 `setdefault` 设 1，游戏环境里的值优先）。
  - 每个线程最新的约 4 KB 符号留在缓冲区里不写出（`JitSymbols.cpp:129` 的时间条件
    `LastWrite - Now < 100ms` 恒成立，只按大小写出），报告里的「fex-jit 中有符号的比例」就是覆盖率。
  - 开命名后 FEX 不再给代码缓冲区起 VMA 名字（`SharedCodeBufferManager.cpp:53-56`），报告按环境变量认出这一场，
    把没有名字的匿名可执行内存算作 `jit:no-symbol`。
- **FEX 统计**：Proton 的 FEX 配置开着 `ProfileStats`，每个线程的计数在共享内存 `fex-<pid>-stats`（guest 里的
  `/dev/shm`）：编译时间和次数、异常处理时间、SIGBUS（非对齐原子）、SMC、x87 回退、查找未命中和锁时间、磁盘缓存命中。
  ARM64EC 下槽位里是 Windows 线程号；进程的第一个线程占第 0 个槽，所以第 0 个槽是主线程。时间单位是 `CNTVCT_EL0`
  的计数，`fexstats.py` 读 `cntfrq_el0` 换算。

## 文件

| 文件 | 作用 |
|---|---|
| `prof-device.sh` | 在主机上跑（adb）：`check [tid]`（系统版本、`perf_event_paranoid`、simpleperf、是否可剖析，给了 tid 就做 1 秒 `simpleperf stat` 证明采得到）、`record TAG TIDS 秒 [事件] [频率] [-- simpleperf 参数]`（`simpleperf record --app`，然后在设备上 `simpleperf dump`、`simpleperf report`，拉回 `$OUT/TAG/`）、`stat TIDS 秒 [事件]`（按线程的硬件计数器）、`clean` |
| `prof-guest.sh` | 在 guest 里跑（`droiddeckctl guest`，或测试工具的 g.sh）：`install`（从 agent-inbox 拷 py 文件）、`env set [block\|library\|none]` / `env unset`（写 `game-environment.json` 的 `games[3167020]`，开会话后、开游戏前做；已有同名项不动）、`ids`、`snap TAG`、`stats [秒]`、`pack TAG`（打包到 `Android/data/<包名>/files/fexprof/`，给 adb pull）、`restore` |
| `guestsnap.py` | `ids`：游戏进程（同名里线程最多的）、主线程（tid == pid）、wineserver、跟踪它的进程（proot）。`snap DIR`：maps、各线程 CPU 节拍、wineserver 和 proot 的进程 CPU、游戏的 FEX/Wine/DXVK 环境变量、FEX 统计、PE 映像（经 `/proc/<pid>/mem` 读映射起点的 PE 头，找 Wine 读进匿名内存的映像）、FEX 的符号文件（gzip） |
| `fexstats.py` | 读 `fex-<pid>-stats`：`snap`、`rate PID --seconds S`、`diff A B` |
| `prof-report.py` | 在主机上跑：`simpleperf dump` 的输出 + 前后两份快照 → 每个线程按类别和模块的样本比例、最热的位置（PE 是 RVA，ELF 是文件偏移，JIT 块是客体模块偏移或客体地址）、/proc 的忙碌和内核占比、wineserver 和 proot 的 CPU、FEX 统计的速率。`--json` 另存结果 |
| `tools/tests/test_fex_prof.py` | 测试（CI 的 Python 测试会跑）：`PYTHONPATH=tools/tests python3 -m unittest test_fex_prof` |

## 设备上怎么做

1. `adb push guestsnap.py fexstats.py prof-report.py /sdcard/Android/data/com.droiddeck.launcher.g2/files/agent-inbox/`；
   guest 里 `prof-guest.sh install`。
2. 开 Steam 会话。要按客体模块分的那一场，开游戏前 `prof-guest.sh env set`（不命名的对照场不做这步或 `env set none`）。
3. 开鸭科夫，进到要测的画面，等稳定后：
   `prof-guest.sh ids`（记下 PID，主线程 tid = PID）→ `prof-guest.sh snap A` →
   `prof-device.sh record r1 <主线程 tid> 30` → `prof-guest.sh snap B` → `prof-guest.sh pack A`、`pack B`，
   `adb pull` 两个包。
4. 主机上：`python3 prof-report.py fexprof-out/r1/r1.dump.txt.gz A B --json r1.json`。
5. 收尾：`prof-guest.sh restore`、`prof-device.sh clean`，删掉 agent-inbox 里的文件。

可选：`record` 后面加 `-- --call-graph fp` 看谁调用了 FEX 运行时、Wine 和 DXVK（`simpleperf report` 的输出里）；
`-- -p <wineserver pid>` 同时采 wineserver。`stat <tid> 10` 看主线程的 IPC、分支和缓存未命中。

## 限制

- 只有在 CPU 上的时间。主线程等 wineserver、等渲染线程的时间看 perfetto 的 `sched_switch`/`sched_waking`（不用 root）。
- App 上下文里的 simpleperf 不能用 tracepoint（没有 `--trace-offcpu`），内核调用链被去掉，内核样本只算一类。
- simpleperf 不解析 PE：libarm64ecfex.dll、Wine 的 PE dll、DXVK 只给 RVA，要符号得另拿带符号的同版本文件对。
- `jit:outside-images` 只有客体地址；Mono 方法名要另外有 Mono 的 JIT 映射。
- 命名的那一场关了磁盘缓存，加载期多编译；稳定后的帧率和主线程类别比例要和不命名的对照场对一下。
