#ifndef ZEROFG_GLUE_H
#define ZEROFG_GLUE_H
#include <stdint.h>
#include <vulkan/vulkan.h>

/* [mali-g2] C interface to ZeroFG (thirdparty/zerofg) for zerofg_zc.c. */

#ifdef __cplusplus
extern "C" {
#endif

enum { ZFG_ST_OK = 0, ZFG_ST_INVALID, ZFG_ST_UNSUPPORTED, ZFG_ST_OOM, ZFG_ST_VULKAN };
enum { ZFG_MODE_REALLYZERO = 0, ZFG_MODE_ZERO = 1 };

/* What is enabled on the device ZeroFG records for. */
struct zfg_caps {
    uint32_t api_version;
    int sync2, storage_extended_formats, fp16_supported, fp16_enabled;
    uint32_t subgroup_size, subgroup_min, subgroup_max, max_workgroup_subgroups;
    VkShaderStageFlags subgroup_stages;
    VkSubgroupFeatureFlags subgroup_ops;
};

struct zfg_build {
    VkInstance instance;
    VkPhysicalDevice pd;
    VkDevice dev;
    PFN_vkGetInstanceProcAddr gipa;
    struct zfg_caps caps;
    int mode;
    int compat_only;
    uint32_t contexts;
    uint32_t width, height;
    VkFormat in_format, out_format;
};

struct zfg_image {
    VkImage image;
    VkImageView view;
    VkImageLayout layout;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
    uint64_t sequence;
};

typedef struct zfg_engine zfg_engine;

/* Create and Resize, with Auto then Compat (unless compat_only). NULL: *status says why. */
zfg_engine *zfg_engine_build(const struct zfg_build *b, int *status, const char **backend);
int zfg_engine_interpolate(zfg_engine *e, VkCommandBuffer cmd, uint32_t context, const struct zfg_image *previous,
                           const struct zfg_image *current, const struct zfg_image *output);
void zfg_engine_destroy(zfg_engine *e);
const char *zfg_status_name(int status);

#ifdef __cplusplus
}
#endif
#endif
