// fg-bench: GPU cost of ZeroFG frame generation (third_party/zerofg, v1.1.0) on the Vulkan device the loader
// gives this process. In the DroidDeck guest that is the in-process Mali driver (mali-g2/hybris) when
// fg-bench-guest.sh sets the hybris ICD up; here (PalmCode's Ubuntu) it is lavapipe. See README.md.
//
// Per size and mode: real frames (a texture that repeats every 512 px, panned by an even number of pixels per
// frame, with a static box over it) are produced on the GPU into a ring of three images, and ZeroFG generates the
// midpoint of each pair into an output image. GPU timestamps bracket only ZeroFG's recorded work. Consecutive pairs
// share a frame (A->B, B->C, ...) with increasing sequence numbers, as a compositor would feed them (--fresh:
// sequence 0, every pair from nothing). After the timed iterations the last pair and its output are read back: the
// check passes when the output's checksum differs from both inputs' and the output is not uniform. Printed beside
// it: the mean error of the output, A, B and the 50/50 blend against the exact midpoint (the texture panned by half
// a step), and a note when the output is one input shown whole (what both engines do on a scene cut).
//
// Usage: fg-bench [--size WxH]... [--mode zero|reallyzero|both] [--iters N] [--warmup N] [--serial]
//                 [--in bgra8|rgba8|a2b10] [--out rgba8|bgra8|a2b10] [--backend auto|compat|modern]
//                 [--motion DX,DY] [--fresh] [--no-fp16] [--device N] [--loader PATH] [--json] [--list]
// Exit: 0 ok, 1 a check failed, 2 no usable device / ZeroFG unsupported, 3 usage, 4 Vulkan error.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "zerofg/zerofg.h"

namespace {

// ---------------------------------------------------------------------------------------------------- Vulkan entry points

PFN_vkGetInstanceProcAddr gipa;
#define GLOBAL_FUNCS(X) X(CreateInstance) X(EnumerateInstanceVersion)
#define INSTANCE_FUNCS(X)                                                                                    \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) \
  X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties)  \
  X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCS(X)                                                                                      \
  X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)      \
  X(BeginCommandBuffer) X(EndCommandBuffer) X(QueueSubmit) X(DeviceWaitIdle) X(CreateFence) X(DestroyFence)    \
  X(WaitForFences) X(ResetFences) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView)        \
  X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer)             \
  X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory)              \
  X(InvalidateMappedMemoryRanges) X(CmdPipelineBarrier2) X(CmdCopyBufferToImage) X(CmdCopyImage)              \
  X(CmdCopyImageToBuffer) X(CmdWriteTimestamp2) X(CmdResetQueryPool) X(CreateQueryPool) X(DestroyQueryPool)    \
  X(GetQueryPoolResults) X(ResetCommandBuffer)
#define DECLARE(n) PFN_vk##n vk##n;
GLOBAL_FUNCS(DECLARE)
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
#undef DECLARE

void* g_loader;
std::string g_loader_name;

bool load_loader(const char* explicit_path) {
  std::vector<std::string> tries;
  if (explicit_path && *explicit_path) tries.push_back(explicit_path);
  tries.push_back("libvulkan.so.1");
  tries.push_back("libvulkan.so");
  for (const std::string& t : tries) {
    g_loader = dlopen(t.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (g_loader) { g_loader_name = t; break; }
    if (explicit_path && *explicit_path) {
      fprintf(stderr, "fg-bench: cannot load %s: %s\n", t.c_str(), dlerror());
      return false;
    }
  }
  if (!g_loader) {
    fprintf(stderr, "fg-bench: no Vulkan loader (libvulkan.so.1); give one with --loader PATH\n");
    return false;
  }
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
  if (!gipa) { fprintf(stderr, "fg-bench: %s has no vkGetInstanceProcAddr\n", g_loader_name.c_str()); return false; }
#define LOAD(n) vk##n = reinterpret_cast<PFN_vk##n>(gipa(VK_NULL_HANDLE, "vk" #n));
  GLOBAL_FUNCS(LOAD)
#undef LOAD
  return vkCreateInstance != nullptr;
}

bool load_instance(VkInstance inst) {
  bool ok = true;
#define LOAD(n) vk##n = reinterpret_cast<PFN_vk##n>(gipa(inst, "vk" #n)); if (!vk##n) { fprintf(stderr, "fg-bench: no vk" #n "\n"); ok = false; }
  INSTANCE_FUNCS(LOAD)
#undef LOAD
  return ok;
}

bool load_device(VkDevice dev) {
  bool ok = true;
#define LOAD(n) vk##n = reinterpret_cast<PFN_vk##n>(vkGetDeviceProcAddr(dev, "vk" #n)); if (!vk##n) { fprintf(stderr, "fg-bench: no vk" #n "\n"); ok = false; }
  DEVICE_FUNCS(LOAD)
#undef LOAD
  return ok;
}

#define VKCHECK(expr)                                                                       \
  do {                                                                                      \
    VkResult r_ = (expr);                                                                   \
    if (r_ != VK_SUCCESS) {                                                                 \
      fprintf(stderr, "fg-bench: %s failed: VkResult %d (line %d)\n", #expr, (int)r_, __LINE__); \
      std::exit(4);                                                                         \
    }                                                                                       \
  } while (0)

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------------------------------- options

struct Size { uint32_t w, h; };

struct Options {
  std::vector<Size> sizes;
  bool zero = true, really = true;
  int iters = 60, warmup = 10;
  bool serial = false, fresh = false, fp16 = true, json = false, list = false;
  VkFormat in = VK_FORMAT_B8G8R8A8_UNORM, out = VK_FORMAT_R8G8B8A8_UNORM;
  zerofg::Backend backend = zerofg::Backend::kAuto;
  int dx = -1, dy = -1;
  int device = -1;
  const char* loader = nullptr;
};

// Digits only; no strtol (keeps the binary off glibc 2.38's __isoc23_strtol).
bool parse_uint(const char*& p, uint32_t& v) {
  if (*p < '0' || *p > '9') return false;
  uint64_t x = 0;
  while (*p >= '0' && *p <= '9') { x = x * 10 + uint64_t(*p++ - '0'); if (x > 0x7fffffff) return false; }
  v = uint32_t(x);
  return true;
}
bool parse_int_arg(const char* s, int& v) {
  uint32_t u;
  if (!parse_uint(s, u) || *s) return false;
  v = int(u);
  return true;
}
bool parse_pair(const char* s, char sep, uint32_t& a, uint32_t& b) {
  return parse_uint(s, a) && *s++ == sep && parse_uint(s, b) && !*s;
}

struct FormatName { const char* name; VkFormat format; };
const FormatName kFormats[] = {
    {"bgra8", VK_FORMAT_B8G8R8A8_UNORM},
    {"rgba8", VK_FORMAT_R8G8B8A8_UNORM},
    {"a2b10", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
};
const char* format_name(VkFormat f) {
  for (const FormatName& n : kFormats) if (n.format == f) return n.name;
  return "?";
}
bool parse_format(const char* s, VkFormat& f) {
  for (const FormatName& n : kFormats) if (!strcmp(s, n.name)) { f = n.format; return true; }
  return false;
}

void usage() {
  fprintf(stderr,
          "usage: fg-bench [--size WxH]... [--mode zero|reallyzero|both] [--iters N] [--warmup N] [--serial]\n"
          "                [--in bgra8|rgba8|a2b10] [--out rgba8|bgra8|a2b10] [--backend auto|compat|modern]\n"
          "                [--motion DX,DY] [--fresh] [--no-fp16] [--device N] [--loader PATH] [--json] [--list]\n"
          "defaults: --size 1704x1136 --size 3408x2272 --mode both --iters 60 --warmup 10 --in bgra8 --out rgba8\n"
          "          --motion: even pixels per real frame, default about width/128 x height/256\n");
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    auto need = [&]() { if (!v) { fprintf(stderr, "fg-bench: %s needs a value\n", a); return false; } ++i; return true; };
    if (!strcmp(a, "--size")) {
      Size s;
      if (!need() || !parse_pair(v, 'x', s.w, s.h) || s.w < 64 || s.h < 64 || s.w > 16384 || s.h > 16384) {
        fprintf(stderr, "fg-bench: --size WxH, 64..16384\n"); return false;
      }
      o.sizes.push_back(s);
    } else if (!strcmp(a, "--mode")) {
      if (!need()) return false;
      if (!strcmp(v, "zero")) { o.zero = true; o.really = false; }
      else if (!strcmp(v, "reallyzero")) { o.zero = false; o.really = true; }
      else if (!strcmp(v, "both")) { o.zero = o.really = true; }
      else { fprintf(stderr, "fg-bench: --mode zero|reallyzero|both\n"); return false; }
    } else if (!strcmp(a, "--iters")) {
      if (!need() || !parse_int_arg(v, o.iters) || o.iters < 1) { fprintf(stderr, "fg-bench: --iters N >= 1\n"); return false; }
    } else if (!strcmp(a, "--warmup")) {
      if (!need() || !parse_int_arg(v, o.warmup)) { fprintf(stderr, "fg-bench: --warmup N\n"); return false; }
    } else if (!strcmp(a, "--in") || !strcmp(a, "--out")) {
      if (!need() || !parse_format(v, a[2] == 'i' ? o.in : o.out)) { fprintf(stderr, "fg-bench: %s bgra8|rgba8|a2b10\n", a); return false; }
    } else if (!strcmp(a, "--backend")) {
      if (!need()) return false;
      if (!strcmp(v, "auto")) o.backend = zerofg::Backend::kAuto;
      else if (!strcmp(v, "compat")) o.backend = zerofg::Backend::kCompat;
      else if (!strcmp(v, "modern")) o.backend = zerofg::Backend::kModern;
      else { fprintf(stderr, "fg-bench: --backend auto|compat|modern\n"); return false; }
    } else if (!strcmp(a, "--motion")) {
      uint32_t x, y;
      if (!need() || !parse_pair(v, ',', x, y) || (x & 1) || (y & 1) || x >= 256 || y >= 256) {
        fprintf(stderr, "fg-bench: --motion DX,DY: even, below 256\n"); return false;
      }
      o.dx = int(x); o.dy = int(y);
    } else if (!strcmp(a, "--device")) {
      if (!need() || !parse_int_arg(v, o.device)) { fprintf(stderr, "fg-bench: --device N\n"); return false; }
    } else if (!strcmp(a, "--loader")) {
      if (!need()) return false;
      o.loader = v;
    } else if (!strcmp(a, "--serial")) o.serial = true;
    else if (!strcmp(a, "--fresh")) o.fresh = true;
    else if (!strcmp(a, "--no-fp16")) o.fp16 = false;
    else if (!strcmp(a, "--json")) o.json = true;
    else if (!strcmp(a, "--list")) o.list = true;
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); std::exit(0); }
    else { fprintf(stderr, "fg-bench: unknown argument %s\n", a); return false; }
  }
  if (o.sizes.empty()) o.sizes = {{1704, 1136}, {3408, 2272}};
  return true;
}

// ---------------------------------------------------------------------------------------------------- pixels

// The scene: a 512 x 512 tile, repeated: value noise at three scales (64, 16 and 8 px lattices, periodic over the
// tile) under scattered squares of random size, place and colour (no regular grid: a periodic pattern lets a motion
// search lock onto the wrong period). Frame s shows it from (s*dx, s*dy), so the exact midpoint of frames s-1 and s
// is the tile from ((s-1)*dx + dx/2, ...); a static box (the "HUD") sits over every frame.
constexpr int kTile = 512;
struct Rgb { uint8_t r, g, b; };
std::vector<Rgb> g_tile;

uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// Value noise on a lattice of `cell` px, periodic over the tile; 0..1.
float vnoise(int x, int y, int cell, uint32_t seed) {
  const int n = kTile / cell;
  const int ix = x / cell, iy = y / cell;
  float fx = float(x % cell) / float(cell), fy = float(y % cell) / float(cell);
  fx = fx * fx * (3.0f - 2.0f * fx);
  fy = fy * fy * (3.0f - 2.0f * fy);
  auto v = [&](int cx, int cy) {
    return float(hash32(uint32_t((cy % n) * n + (cx % n)) * 2654435761U + seed) & 0xffff) / 65535.0f;
  };
  const float a = v(ix, iy), b = v(ix + 1, iy), c = v(ix, iy + 1), d = v(ix + 1, iy + 1);
  return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
}

void build_tile() {
  g_tile.resize(size_t(kTile) * kTile);
  for (int y = 0; y < kTile; ++y) {
    for (int x = 0; x < kTile; ++x) {
      uint8_t c[3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t seed = 0x9e3779b9U * uint32_t(k + 1);
        const float v = 0.55f * vnoise(x, y, 64, seed) + 0.30f * vnoise(x, y, 16, seed ^ 0x51ed27U) +
                        0.15f * vnoise(x, y, 8, seed ^ 0xa5a5a5U);
        c[k] = uint8_t(std::min(255.0f, std::max(0.0f, 20.0f + v * 215.0f)));
      }
      g_tile[size_t(y) * kTile + x] = {c[0], c[1], c[2]};
    }
  }
  // 160 squares, 6..45 px, drawn in order (later ones cover earlier ones), wrapping over the tile's edges.
  for (uint32_t i = 0; i < 160; ++i) {
    const uint32_t h = hash32(i * 0x632be5abU + 17), h2 = hash32(h ^ 0x85ebca6bU);
    const int x0 = int(h % kTile), y0 = int((h >> 9) % kTile), size = 6 + int((h >> 18) % 40);
    const Rgb col = {uint8_t(h2), uint8_t(h2 >> 8), uint8_t(h2 >> 16)};
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) g_tile[size_t((y0 + y) & (kTile - 1)) * kTile + size_t((x0 + x) & (kTile - 1))] = col;
  }
}

Rgb tile_at(int x, int y) { return g_tile[size_t(y & (kTile - 1)) * kTile + size_t(x & (kTile - 1))]; }

// The static box: thin bright bars, like text on a panel.
Rgb hud_at(int x, int y) {
  const bool bar = (y % 12) < 3 && (x % 40) < 30;
  return bar ? Rgb{250, 250, 235} : Rgb{20, 24, 36};
}

uint32_t pack(VkFormat f, Rgb c) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return uint32_t(c.b) | uint32_t(c.g) << 8 | uint32_t(c.r) << 16 | 0xff000000u;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      auto t = [](uint8_t v) { return uint32_t((v * 1023 + 127) / 255); };
      return t(c.r) | t(c.g) << 10 | t(c.b) << 20 | 3u << 30;
    }
    default: return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | 0xff000000u;
  }
}

// Back to 0..255 per channel (10-bit formats keep their fraction).
void unpack(VkFormat f, uint32_t p, float rgb[3]) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
      rgb[0] = float((p >> 16) & 0xff); rgb[1] = float((p >> 8) & 0xff); rgb[2] = float(p & 0xff); break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      rgb[0] = float(p & 0x3ff) * 255.0f / 1023.0f; rgb[1] = float((p >> 10) & 0x3ff) * 255.0f / 1023.0f;
      rgb[2] = float((p >> 20) & 0x3ff) * 255.0f / 1023.0f; break;
    default:
      rgb[0] = float(p & 0xff); rgb[1] = float((p >> 8) & 0xff); rgb[2] = float((p >> 16) & 0xff); break;
  }
}

// ---------------------------------------------------------------------------------------------------- device

struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties mem{};
  uint32_t ts_bits = 0;
  float ts_period = 0.0f;
  zerofg::Capabilities caps;
  VkCommandPool pool = VK_NULL_HANDLE;
};
Gpu G;

const char* device_type(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "other";
  }
}

void api_text(uint32_t v, char* out, size_t n) {
  snprintf(out, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

int create_gpu(const Options& o) {
  uint32_t loader_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader_version);
  char lv[32];
  api_text(loader_version, lv, sizeof(lv));
  const char* icd = getenv("VK_ICD_FILENAMES");
  if (!icd) icd = getenv("VK_DRIVER_FILES");
  printf("loader: %s (instance version %s), ICD list %s\n", g_loader_name.c_str(), lv, icd ? icd : "(loader default)");
  if (loader_version < VK_API_VERSION_1_3) {
    fprintf(stderr, "fg-bench: the loader offers Vulkan %s; ZeroFG needs 1.3\n", lv);
    return 2;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "fg-bench";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VKCHECK(vkCreateInstance(&ici, nullptr, &G.instance));
  if (!load_instance(G.instance)) return 4;

  uint32_t n = 0;
  VKCHECK(vkEnumeratePhysicalDevices(G.instance, &n, nullptr));
  std::vector<VkPhysicalDevice> pds(n);
  if (n) VKCHECK(vkEnumeratePhysicalDevices(G.instance, &n, pds.data()));
  if (!n) { fprintf(stderr, "fg-bench: no Vulkan device\n"); return 2; }
  int pick = -1;
  for (uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pds[i], &p2);
    char av[32];
    api_text(p2.properties.apiVersion, av, sizeof(av));
    printf("device %u: %s (%s), Vulkan %s, driver %s %s\n", i, p2.properties.deviceName,
           device_type(p2.properties.deviceType), av, drv.driverName, drv.driverInfo);
    if (o.device < 0 && pick < 0 && p2.properties.apiVersion >= VK_API_VERSION_1_3) pick = int(i);
  }
  if (o.device >= 0) pick = o.device < int(n) ? o.device : -1;
  if (pick < 0) { fprintf(stderr, "fg-bench: no device with Vulkan 1.3 (or --device out of range)\n"); return 2; }
  G.pd = pds[size_t(pick)];
  vkGetPhysicalDeviceProperties(G.pd, &G.props);
  vkGetPhysicalDeviceMemoryProperties(G.pd, &G.mem);
  if (o.list) return -1;
  if (G.props.apiVersion < VK_API_VERSION_1_3) {
    fprintf(stderr, "fg-bench: device %d offers Vulkan below 1.3; ZeroFG needs 1.3\n", pick);
    return 2;
  }

  // A queue family with graphics (blits need it: ZeroFG writes a non-A2B10G10R10 output through a blit), as the
  // compositor's queue is.
  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(G.pd, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(G.pd, &nq, qf.data());
  int fam = -1;
  for (uint32_t i = 0; i < nq; ++i) {
    const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if ((qf[i].queueFlags & want) == want) { fam = int(i); break; }
  }
  if (fam < 0) { fprintf(stderr, "fg-bench: no graphics+compute queue family\n"); return 2; }
  G.qfam = uint32_t(fam);
  G.ts_bits = qf[size_t(fam)].timestampValidBits;
  G.ts_period = G.props.limits.timestampPeriod;

  VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13};
  VkPhysicalDeviceFeatures2 sf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s12};
  vkGetPhysicalDeviceFeatures2(G.pd, &sf);
  VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
  VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p13};
  VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
  vkGetPhysicalDeviceProperties2(G.pd, &pp);

  if (!s13.synchronization2 || !sf.features.shaderStorageImageExtendedFormats) {
    fprintf(stderr, "fg-bench: the device lacks %s, which ZeroFG requires\n",
            !s13.synchronization2 ? "synchronization2" : "shaderStorageImageExtendedFormats");
    return 2;
  }
  VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  e13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
  e12.shaderFloat16 = (o.fp16 && s12.shaderFloat16) ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceFeatures2 ef{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
  ef.features.shaderStorageImageExtendedFormats = VK_TRUE;
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = G.qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VKCHECK(vkCreateDevice(G.pd, &dci, nullptr, &G.dev));
  if (!load_device(G.dev)) return 4;
  vkGetDeviceQueue(G.dev, G.qfam, 0, &G.queue);
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = G.qfam;
  VKCHECK(vkCreateCommandPool(G.dev, &cpi, nullptr, &G.pool));

  zerofg::Capabilities& c = G.caps;
  c.effective_api_version = std::min(G.props.apiVersion, uint32_t(VK_API_VERSION_1_3));
  c.synchronization2_enabled = true;
  c.shader_storage_image_extended_formats_enabled = true;
  c.shader_float16_supported = s12.shaderFloat16;
  c.shader_float16_enabled = e12.shaderFloat16;
  c.subgroup_size = p11.subgroupSize;
  c.subgroup_supported_stages = p11.subgroupSupportedStages;
  c.subgroup_supported_operations = p11.subgroupSupportedOperations;
  c.subgroup_min_size = p13.minSubgroupSize;
  c.subgroup_max_size = p13.maxSubgroupSize;
  c.max_compute_workgroup_subgroups = p13.maxComputeWorkgroupSubgroups;

  char av[32];
  api_text(G.props.apiVersion, av, sizeof(av));
  printf("using device %d: %s, Vulkan %s, queue family %u, timestamps %u bits x %.3f ns, subgroup %u (%u..%u), "
         "fp16 %s\n", pick, G.props.deviceName, av, G.qfam, G.ts_bits, double(G.ts_period), p11.subgroupSize,
         p13.minSubgroupSize, p13.maxSubgroupSize, e12.shaderFloat16 ? "on" : (s12.shaderFloat16 ? "off (--no-fp16)" : "unsupported"));
  return 0;
}

int memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0) {
  for (int pass = 0; pass < 2; ++pass) {
    const VkMemoryPropertyFlags w = pass == 0 ? (want | nice) : want;
    for (uint32_t i = 0; i < G.mem.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (G.mem.memoryTypes[i].propertyFlags & w) == w) return int(i);
  }
  return -1;
}

struct Img {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  uint32_t w = 0, h = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

Img make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, bool view) {
  Img im;
  im.format = f; im.usage = usage; im.w = w; im.h = h;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = f;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKCHECK(vkCreateImage(G.dev, &ci, nullptr, &im.image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(G.dev, im.image, &req);
  const int t = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (t < 0) { fprintf(stderr, "fg-bench: no device-local memory for an image\n"); std::exit(4); }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = uint32_t(t);
  VKCHECK(vkAllocateMemory(G.dev, &ai, nullptr, &im.mem));
  VKCHECK(vkBindImageMemory(G.dev, im.image, im.mem, 0));
  if (view) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(G.dev, &vi, nullptr, &im.view));
  }
  return im;
}

void free_image(Img& im) {
  if (im.view) vkDestroyImageView(G.dev, im.view, nullptr);
  if (im.image) vkDestroyImage(G.dev, im.image, nullptr);
  if (im.mem) vkFreeMemory(G.dev, im.mem, nullptr);
  im = Img{};
}

struct Buf {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
  VkDeviceSize size = 0;
  bool coherent = false;
};

Buf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
  Buf b;
  b.size = size;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKCHECK(vkCreateBuffer(G.dev, &ci, nullptr, &b.buf));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(G.dev, b.buf, &req);
  const int t = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (t < 0) { fprintf(stderr, "fg-bench: no host-visible memory\n"); std::exit(4); }
  b.coherent = (G.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = uint32_t(t);
  VKCHECK(vkAllocateMemory(G.dev, &ai, nullptr, &b.mem));
  VKCHECK(vkBindBufferMemory(G.dev, b.buf, b.mem, 0));
  VKCHECK(vkMapMemory(G.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
  return b;
}

void free_buffer(Buf& b) {
  if (b.map) vkUnmapMemory(G.dev, b.mem);
  if (b.buf) vkDestroyBuffer(G.dev, b.buf, nullptr);
  if (b.mem) vkFreeMemory(G.dev, b.mem, nullptr);
  b = Buf{};
}

void invalidate(const Buf& b) {
  if (b.coherent) return;
  VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  r.memory = b.mem;
  r.offset = 0;
  r.size = VK_WHOLE_SIZE;
  VKCHECK(vkInvalidateMappedMemoryRanges(G.dev, 1, &r));
}

// Layout change with a full execution and memory dependency on everything before (the benchmark's own copies
// and ZeroFG's work); cheap next to what is measured, and outside the timestamps.
void transition(VkCommandBuffer cmd, Img& im, VkImageLayout to, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = dst_stage;
  b.dstAccessMask = dst_access;
  b.oldLayout = im.layout;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &di);
  im.layout = to;
}

struct OneShot {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};
OneShot g_once;

VkCommandBuffer once_begin() {
  if (!g_once.cmd) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = G.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(G.dev, &ai, &g_once.cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(G.dev, &fi, nullptr, &g_once.fence));
  }
  VKCHECK(vkResetCommandBuffer(g_once.cmd, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKCHECK(vkBeginCommandBuffer(g_once.cmd, &bi));
  return g_once.cmd;
}

void once_submit_wait() {
  VKCHECK(vkEndCommandBuffer(g_once.cmd));
  VKCHECK(vkResetFences(G.dev, 1, &g_once.fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &g_once.cmd;
  VKCHECK(vkQueueSubmit(G.queue, 1, &si, g_once.fence));
  VKCHECK(vkWaitForFences(G.dev, 1, &g_once.fence, VK_TRUE, UINT64_MAX));
}

// ---------------------------------------------------------------------------------------------------- the scene on the GPU

struct Scene {
  uint32_t w = 0, h = 0;
  int dx = 0, dy = 0;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  Img world;          // (w + kTile) x (h + kTile): the tile repeated
  Img hud;            // the static box
  int hud_x = 0, hud_y = 0;
  Img ring[3];        // real frames
};

int frame_ox(const Scene& s, int64_t seq) { return int((seq * s.dx) % kTile); }
int frame_oy(const Scene& s, int64_t seq) { return int((seq * s.dy) % kTile); }

void upload(Img& dst, const std::vector<uint32_t>& px) {
  Buf st = make_buffer(VkDeviceSize(px.size()) * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
  memcpy(st.map, px.data(), px.size() * 4);
  if (!st.coherent) {
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = st.mem; r.size = VK_WHOLE_SIZE;
    PFN_vkFlushMappedMemoryRanges flush =
        reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(vkGetDeviceProcAddr(G.dev, "vkFlushMappedMemoryRanges"));
    if (flush) VKCHECK(flush(G.dev, 1, &r));
  }
  VkCommandBuffer cmd = once_begin();
  transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  VkBufferImageCopy r{};
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {dst.w, dst.h, 1};
  vkCmdCopyBufferToImage(cmd, st.buf, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  once_submit_wait();
  free_buffer(st);
}

void make_scene(Scene& s, uint32_t w, uint32_t h, VkFormat fmt, int dx, int dy) {
  s.w = w; s.h = h; s.fmt = fmt;
  auto even = [](double v) { int i = int(v + 0.5); i &= ~1; return std::max(2, std::min(254, i)); };
  s.dx = dx >= 0 ? dx : even(w / 128.0);
  s.dy = dy >= 0 ? dy : even(h / 256.0);
  const VkImageUsageFlags copy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  s.world = make_image(w + kTile, h + kTile, fmt, copy, false);
  std::vector<uint32_t> px(size_t(w + kTile) * (h + kTile));
  for (uint32_t y = 0; y < h + kTile; ++y)
    for (uint32_t x = 0; x < w + kTile; ++x) px[size_t(y) * (w + kTile) + x] = pack(fmt, tile_at(int(x), int(y)));
  upload(s.world, px);
  const uint32_t hw = std::max(64u, w / 5), hh = std::max(24u, h / 10);
  s.hud_x = int(w / 40); s.hud_y = int(h / 30);
  s.hud = make_image(hw, hh, fmt, copy, false);
  px.assign(size_t(hw) * hh, 0);
  for (uint32_t y = 0; y < hh; ++y)
    for (uint32_t x = 0; x < hw; ++x) px[size_t(y) * hw + x] = pack(fmt, hud_at(int(x), int(y)));
  upload(s.hud, px);
  for (Img& r : s.ring) r = make_image(w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | copy, true);
}

void free_scene(Scene& s) {
  free_image(s.world);
  free_image(s.hud);
  for (Img& r : s.ring) free_image(r);
}

// Records real frame `seq` into ring[seq % 3], ending in SHADER_READ_ONLY_OPTIMAL. The barrier before the copy also
// orders it after ZeroFG's earlier reads of that slot (same queue, submission order).
void record_frame(VkCommandBuffer cmd, Scene& s, int64_t seq) {
  Img& dst = s.ring[seq % 3];
  transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  VkImageCopy c{};
  c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.srcOffset = {frame_ox(s, seq), frame_oy(s, seq), 0};
  c.extent = {s.w, s.h, 1};
  vkCmdCopyImage(cmd, s.world.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &di);
  VkImageCopy hc{};
  hc.srcSubresource = hc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  hc.dstOffset = {s.hud_x, s.hud_y, 0};
  hc.extent = {s.hud.w, s.hud.h, 1};
  vkCmdCopyImage(cmd, s.hud.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hc);
  transition(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
}

zerofg::Image wrap(const Img& im, uint64_t sequence) {
  zerofg::Image z;
  z.image = im.image;
  z.view = im.view;
  z.layout = im.layout;
  z.format = im.format;
  z.width = im.w;
  z.height = im.h;
  z.usage = im.usage;
  z.tiling = VK_IMAGE_TILING_OPTIMAL;
  z.create_flags = 0;
  z.view_type = VK_IMAGE_VIEW_TYPE_2D;
  z.creation_facts_known = true;
  z.sequence = sequence;
  return z;
}

// ---------------------------------------------------------------------------------------------------- one run

const char* status_name(zerofg::Status s) {
  switch (s) {
    case zerofg::Status::kSuccess: return "success";
    case zerofg::Status::kInvalidArgument: return "invalid argument";
    case zerofg::Status::kUnsupported: return "unsupported";
    case zerofg::Status::kOutOfMemory: return "out of memory";
    case zerofg::Status::kVulkanError: return "Vulkan error";
  }
  return "?";
}

struct Stats { double median = 0, p90 = 0, min = 0, max = 0, mean = 0; size_t n = 0; };
Stats stats(std::vector<double> v) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  auto q = [&](double p) {  // linear interpolation between closest ranks
    const double pos = p * double(v.size() - 1);
    const size_t i = size_t(pos);
    const double f = pos - double(i);
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
  };
  s.median = q(0.5);
  s.p90 = q(0.9);
  s.min = v.front();
  s.max = v.back();
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / double(v.size());
  return s;
}

uint64_t fnv64(const uint32_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n * 4; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

struct Check {
  bool ok = false;
  uint64_t sum_a = 0, sum_b = 0, sum_out = 0;
  bool uniform = true;
  double mae_out = 0, mae_a = 0, mae_b = 0, mae_blend = 0;
  double out_vs_a = 0, out_vs_b = 0;  // mean abs difference of the output from each input
  const char* near_copy = nullptr;    // "A"/"B": the output is (almost) that input, as for a scene cut
  bool nearer_than_blend = false;     // the output is nearer the exact midpoint than the 50/50 blend
  uint32_t margin = 0;
};

// The last pair and its output, read back and compared with the exact midpoint (inside a margin the frames'
// motion brings in from outside, which neither frame shows).
Check check_last(Scene& s, Img& out, int64_t seq_b) {
  Check c;
  const size_t n = size_t(s.w) * s.h;
  Img* imgs[3] = {&s.ring[(seq_b - 1) % 3], &s.ring[seq_b % 3], &out};
  Buf bufs[3];
  VkCommandBuffer cmd = once_begin();
  for (int i = 0; i < 3; ++i) {
    bufs[i] = make_buffer(VkDeviceSize(n) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {s.w, s.h, 1};
    vkCmdCopyImageToBuffer(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bufs[i].buf, 1, &r);
  }
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &di);
  once_submit_wait();
  for (Buf& b : bufs) invalidate(b);
  const uint32_t* pa = static_cast<const uint32_t*>(bufs[0].map);
  const uint32_t* pb = static_cast<const uint32_t*>(bufs[1].map);
  const uint32_t* po = static_cast<const uint32_t*>(bufs[2].map);
  c.sum_a = fnv64(pa, n);
  c.sum_b = fnv64(pb, n);
  c.sum_out = fnv64(po, n);
  const uint32_t first = po[0];
  for (size_t i = 1; i < n && c.uniform; ++i) c.uniform = po[i] == first;

  const int mx = (seq_b - 1) * s.dx % kTile + s.dx / 2, my = (seq_b - 1) * s.dy % kTile + s.dy / 2;
  c.margin = uint32_t(std::max(s.dx, s.dy) + 8);
  double ea = 0, eb = 0, eo = 0, ebl = 0, oa = 0, ob = 0;
  uint64_t count = 0;
  const VkFormat of = out.format;
  for (uint32_t y = c.margin; y + c.margin < s.h; ++y) {
    for (uint32_t x = c.margin; x + c.margin < s.w; ++x) {
      const bool in_hud = int(x) >= s.hud_x && int(x) < s.hud_x + int(s.hud.w) && int(y) >= s.hud_y && int(y) < s.hud_y + int(s.hud.h);
      const Rgb m = in_hud ? hud_at(int(x) - s.hud_x, int(y) - s.hud_y) : tile_at(int(x) + mx, int(y) + my);
      const float mid[3] = {float(m.r), float(m.g), float(m.b)};
      const size_t i = size_t(y) * s.w + x;
      float a[3], b[3], o[3];
      unpack(s.fmt, pa[i], a);
      unpack(s.fmt, pb[i], b);
      unpack(of, po[i], o);
      for (int k = 0; k < 3; ++k) {
        ea += std::fabs(a[k] - mid[k]);
        eb += std::fabs(b[k] - mid[k]);
        eo += std::fabs(o[k] - mid[k]);
        ebl += std::fabs(0.5f * (a[k] + b[k]) - mid[k]);
        oa += std::fabs(o[k] - a[k]);
        ob += std::fabs(o[k] - b[k]);
      }
      count += 3;
    }
  }
  if (count) {
    c.mae_a = ea / double(count); c.mae_b = eb / double(count); c.mae_out = eo / double(count);
    c.mae_blend = ebl / double(count); c.out_vs_a = oa / double(count); c.out_vs_b = ob / double(count);
  }
  // Within one 8-bit step of an input on average, while the inputs differ by much more: that input shown whole.
  if (c.mae_a > 4.0 && std::min(c.out_vs_a, c.out_vs_b) < 1.0) c.near_copy = c.out_vs_a < c.out_vs_b ? "A" : "B";
  c.nearer_than_blend = c.mae_out < c.mae_blend;
  for (Buf& b : bufs) free_buffer(b);
  c.ok = c.sum_out != c.sum_a && c.sum_out != c.sum_b && !c.uniform;
  return c;
}

struct Frame {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  int64_t iter = -1;
};

constexpr uint32_t kContexts = 3;

struct Result {
  bool ran = false;
  zerofg::Status status = zerofg::Status::kSuccess;
  const char* backend = "";
  double resize_ms = 0, first_gpu_ms = -1, wall_ms_per_frame = 0;
  Stats gpu, wall, copy;
  Check check;
};

int run_one(const Options& o, Scene& s, zerofg::Mode mode, Result& res) {
  // ZeroFG: Auto first; with Auto, a failed create or Resize is retried once on Compat (INTEGRATION.md 1).
  std::unique_ptr<zerofg::Interpolator> fg;
  zerofg::Backend tries[2] = {o.backend, zerofg::Backend::kCompat};
  const int ntries = o.backend == zerofg::Backend::kAuto ? 2 : 1;
  for (int t = 0; t < ntries; ++t) {
    zerofg::CreateInfo info;
    info.vulkan.instance = G.instance;
    info.vulkan.physical_device = G.pd;
    info.vulkan.device = G.dev;
    info.vulkan.get_instance_proc_addr = gipa;
    info.mode = mode;
    info.backend = tries[t];
    info.capabilities = G.caps;
    info.frame_context_count = kContexts;
    zerofg::Status st = zerofg::Status::kSuccess;
    fg = zerofg::Interpolator::Create(info, &st);
    if (fg) {
      const double t0 = now_ms();
      st = fg->Resize(s.w, s.h, s.fmt, o.out);
      res.resize_ms = now_ms() - t0;
      if (st != zerofg::Status::kSuccess) fg.reset();
    }
    res.status = st;
    res.backend = tries[t] == zerofg::Backend::kCompat ? "compat" : tries[t] == zerofg::Backend::kModern ? "modern" : "auto";
    if (fg) break;
    fprintf(stderr, "fg-bench: %s with backend %s: %s\n", mode == zerofg::Mode::kZero ? "Zero" : "ReallyZero", res.backend,
            status_name(st));
  }
  if (!fg) return 2;

  // Outputs: one per frame context (a context is reused only after its submission completed).
  VkImageUsageFlags out_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (o.out == VK_FORMAT_A2B10G10R10_UNORM_PACK32) out_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  Img outs[kContexts];
  for (Img& im : outs) im = make_image(s.w, s.h, o.out, out_usage, true);
  {
    VkCommandBuffer cmd = once_begin();
    for (Img& im : outs) transition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);
    record_frame(cmd, s, 0);
    once_submit_wait();
  }

  VkQueryPool qp = VK_NULL_HANDLE;
  const bool ts = G.ts_bits > 0 && G.ts_period > 0.0f;
  if (ts) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 3 * kContexts;
    VKCHECK(vkCreateQueryPool(G.dev, &qi, nullptr, &qp));
  }
  const uint64_t ts_mask = G.ts_bits >= 64 ? ~0ull : ((1ull << G.ts_bits) - 1);
  Frame frames[kContexts];
  for (Frame& f : frames) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = G.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(G.dev, &ai, &f.cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(G.dev, &fi, nullptr, &f.fence));
  }

  std::vector<double> gpu, wall, copy;
  const int total = o.warmup + o.iters;
  double t_measure = 0;
  auto collect = [&](uint32_t ctx) {
    Frame& f = frames[ctx];
    if (!f.pending) return;
    VKCHECK(vkWaitForFences(G.dev, 1, &f.fence, VK_TRUE, UINT64_MAX));
    f.pending = false;
    if (!ts) return;
    uint64_t q[3] = {0, 0, 0};
    VKCHECK(vkGetQueryPoolResults(G.dev, qp, 3 * ctx, 3, sizeof(q), q, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    const double ms = double((q[2] - q[1]) & ts_mask) * double(G.ts_period) / 1e6;
    const double copy_ms = double((q[1] - q[0]) & ts_mask) * double(G.ts_period) / 1e6;
    if (f.iter == 1) res.first_gpu_ms = ms;
    if (f.iter > o.warmup) { gpu.push_back(ms); copy.push_back(copy_ms); }
  };

  zerofg::Status st = zerofg::Status::kSuccess;
  for (int64_t i = 1; i <= total && st == zerofg::Status::kSuccess; ++i) {
    const uint32_t ctx = uint32_t(i % kContexts);
    collect(ctx);
    if (i == o.warmup + 1) t_measure = now_ms();
    const double t_iter = now_ms();
    Frame& f = frames[ctx];
    VKCHECK(vkResetCommandBuffer(f.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(f.cmd, &bi));
    // ALL_COMMANDS timestamps: each is written once everything before it (earlier submissions too) is done, so
    // the first pair brackets the real frame's copy and the second ZeroFG's work alone.
    if (ts) {
      vkCmdResetQueryPool(f.cmd, qp, 3 * ctx, 3);
      vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 3 * ctx);
    }
    record_frame(f.cmd, s, i);
    if (ts) vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 3 * ctx + 1);
    const zerofg::Image prev = wrap(s.ring[(i - 1) % 3], o.fresh ? 0 : uint64_t(i));
    const zerofg::Image cur = wrap(s.ring[i % 3], o.fresh ? 0 : uint64_t(i + 1));
    st = fg->Interpolate(f.cmd, ctx, prev, cur, 0.5f, wrap(outs[ctx], 0));
    if (ts) vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 3 * ctx + 2);
    VKCHECK(vkEndCommandBuffer(f.cmd));
    if (st != zerofg::Status::kSuccess) {
      fprintf(stderr, "fg-bench: Interpolate: %s\n", status_name(st));
      break;
    }
    VKCHECK(vkResetFences(G.dev, 1, &f.fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    VKCHECK(vkQueueSubmit(G.queue, 1, &si, f.fence));
    f.pending = true;
    f.iter = i;
    if (o.serial) {
      collect(ctx);
      if (i > o.warmup) wall.push_back(now_ms() - t_iter);
    }
  }
  for (uint32_t c = 0; c < kContexts; ++c) collect(c);
  const double t_end = now_ms();
  VKCHECK(vkDeviceWaitIdle(G.dev));
  res.status = st;
  if (st == zerofg::Status::kSuccess) {
    res.ran = true;
    res.gpu = stats(gpu);
    res.wall = stats(wall);
    res.copy = stats(copy);
    res.wall_ms_per_frame = (t_end - t_measure) / double(o.iters);
    res.check = check_last(s, outs[total % kContexts], total);
  }
  fg.reset();
  for (Frame& f : frames) vkDestroyFence(G.dev, f.fence, nullptr);
  // the command buffers go with the pool
  if (qp) vkDestroyQueryPool(G.dev, qp, nullptr);
  for (Img& im : outs) free_image(im);
  return st == zerofg::Status::kSuccess ? 0 : 4;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Options o;
  if (!parse(argc, argv, o)) { usage(); return 3; }
  const char* env_loader = getenv("FGBENCH_LOADER");
  if (!o.loader && env_loader && *env_loader) o.loader = env_loader;
  if (!load_loader(o.loader)) return 2;
  int r = create_gpu(o);
  if (r < 0) return 0;  // --list
  if (r) return r;
  printf("ZeroFG 1.1.0; input %s, output %s, %s, %d+%d iterations, %s sequence\n", format_name(o.in), format_name(o.out),
         o.serial ? "serial (wait after each generation)" : "pipelined (up to 3 generations in flight)", o.warmup, o.iters,
         o.fresh ? "unknown (0: every pair from nothing)" : "consecutive (A->B, B->C, ...)");
  build_tile();
  int worst = 0;
  for (const Size& sz : o.sizes) {
    Scene s;
    make_scene(s, sz.w, sz.h, o.in, o.dx, o.dy);
    for (int m = 0; m < 2; ++m) {
      const zerofg::Mode mode = m == 0 ? zerofg::Mode::kReallyZero : zerofg::Mode::kZero;
      if ((mode == zerofg::Mode::kZero && !o.zero) || (mode == zerofg::Mode::kReallyZero && !o.really)) continue;
      const char* mname = mode == zerofg::Mode::kZero ? "Zero" : "ReallyZero";
      Result res;
      const int rc = run_one(o, s, mode, res);
      if (rc) {
        printf("%ux%u %-10s FAILED: %s\n", sz.w, sz.h, mname, status_name(res.status));
        if (o.json)
          printf("FGBENCH_JSON {\"w\":%u,\"h\":%u,\"mode\":\"%s\",\"ok\":false,\"status\":\"%s\"}\n", sz.w, sz.h, mname,
                 status_name(res.status));
        worst = std::max(worst, rc);
        continue;
      }
      const Check& c = res.check;
      if (res.gpu.n)
        printf("%ux%u %-10s GPU ms/generated frame: median %.3f  p90 %.3f  (min %.3f max %.3f mean %.3f, n=%zu, first %.3f)\n",
               sz.w, sz.h, mname, res.gpu.median, res.gpu.p90, res.gpu.min, res.gpu.max, res.gpu.mean, res.gpu.n, res.first_gpu_ms);
      else
        printf("%ux%u %-10s GPU timestamps unavailable on this queue\n", sz.w, sz.h, mname);
      printf("%ux%u %-10s backend %s, Resize %.1f ms, real-frame copy (GPU) median %.3f ms, wall %.3f ms/frame incl. "
             "that copy%s", sz.w, sz.h, mname, res.backend, res.resize_ms, res.copy.median, res.wall_ms_per_frame,
             o.serial ? "" : "\n");
      if (o.serial) printf(", serial submit->done median %.3f p90 %.3f\n", res.wall.median, res.wall.p90);
      printf("%ux%u %-10s check %s: output %016llx, A %016llx, B %016llx%s; mean abs error vs exact midpoint "
             "(0..255, %u px margin, motion %d,%d): output %.2f, blend %.2f, A %.2f, B %.2f; output vs A %.2f, vs B %.2f%s%s\n",
             sz.w, sz.h, mname, c.ok ? "ok" : "FAILED", (unsigned long long)c.sum_out, (unsigned long long)c.sum_a,
             (unsigned long long)c.sum_b, c.uniform ? ", output uniform" : "", c.margin, s.dx, s.dy, c.mae_out, c.mae_blend,
             c.mae_a, c.mae_b, c.out_vs_a, c.out_vs_b,
             c.near_copy ? (c.near_copy[0] == 'A' ? " -- NOTE: the output is frame A shown whole (a cut to the engine)"
                                                  : " -- NOTE: the output is frame B shown whole (a cut to the engine)") : "",
             !c.near_copy && !c.nearer_than_blend ? " -- NOTE: no nearer the midpoint than a plain blend" : "");
      if (o.json)
        printf("FGBENCH_JSON {\"w\":%u,\"h\":%u,\"mode\":\"%s\",\"ok\":%s,\"backend\":\"%s\",\"in\":\"%s\",\"out\":\"%s\","
               "\"pipelined\":%s,\"fresh\":%s,\"iters\":%d,\"gpu_median_ms\":%.4f,\"gpu_p90_ms\":%.4f,\"gpu_min_ms\":%.4f,"
               "\"gpu_max_ms\":%.4f,\"gpu_first_ms\":%.4f,\"copy_median_ms\":%.4f,\"resize_ms\":%.2f,\"wall_ms_per_frame\":%.4f,"
               "\"mae_out\":%.3f,\"mae_blend\":%.3f,\"mae_a\":%.3f,\"mae_b\":%.3f,\"out_vs_a\":%.3f,\"out_vs_b\":%.3f,"
               "\"near_copy\":%s%s%s,\"nearer_than_blend\":%s,\"motion\":[%d,%d],\"device\":\"%s\"}\n",
               sz.w, sz.h, mname, c.ok ? "true" : "false", res.backend, format_name(o.in), format_name(o.out),
               o.serial ? "false" : "true", o.fresh ? "true" : "false", o.iters, res.gpu.median, res.gpu.p90, res.gpu.min,
               res.gpu.max, res.first_gpu_ms, res.copy.median, res.resize_ms, res.wall_ms_per_frame, c.mae_out, c.mae_blend, c.mae_a,
               c.mae_b, c.out_vs_a, c.out_vs_b, c.near_copy ? "\"" : "", c.near_copy ? c.near_copy : "null",
               c.near_copy ? "\"" : "", c.nearer_than_blend ? "true" : "false", s.dx, s.dy, G.props.deviceName);
      if (!c.ok) worst = std::max(worst, 1);
    }
    free_scene(s);
  }
  if (g_once.fence) vkDestroyFence(G.dev, g_once.fence, nullptr);
  vkDestroyCommandPool(G.dev, G.pool, nullptr);
  vkDestroyDevice(G.dev, nullptr);
  vkDestroyInstance(G.instance, nullptr);
  return worst;
}
