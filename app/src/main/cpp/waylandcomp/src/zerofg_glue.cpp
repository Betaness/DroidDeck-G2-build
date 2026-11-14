#include "zerofg_glue.h"

#include <memory>

#include "zerofg/zerofg.h"

struct zfg_engine {
    std::unique_ptr<zerofg::Interpolator> fg;
};

namespace {

int status_of(zerofg::Status s) {
    switch (s) {
        case zerofg::Status::kSuccess: return ZFG_ST_OK;
        case zerofg::Status::kInvalidArgument: return ZFG_ST_INVALID;
        case zerofg::Status::kUnsupported: return ZFG_ST_UNSUPPORTED;
        case zerofg::Status::kOutOfMemory: return ZFG_ST_OOM;
        case zerofg::Status::kVulkanError: return ZFG_ST_VULKAN;
    }
    return ZFG_ST_VULKAN;
}

zerofg::Image wrap(const zfg_image &i) {
    zerofg::Image z;
    z.image = i.image;
    z.view = i.view;
    z.layout = i.layout;
    z.format = i.format;
    z.width = i.width;
    z.height = i.height;
    z.usage = i.usage;
    z.tiling = VK_IMAGE_TILING_OPTIMAL;
    z.create_flags = 0;
    z.view_type = VK_IMAGE_VIEW_TYPE_2D;
    z.creation_facts_known = true;
    z.sequence = i.sequence;
    return z;
}

}  // namespace

extern "C" {

const char *zfg_status_name(int status) {
    switch (status) {
        case ZFG_ST_OK: return "success";
        case ZFG_ST_INVALID: return "invalid argument";
        case ZFG_ST_UNSUPPORTED: return "unsupported";
        case ZFG_ST_OOM: return "out of memory";
        default: return "Vulkan error";
    }
}

zfg_engine *zfg_engine_build(const struct zfg_build *b, int *status, const char **backend) {
    const zerofg::Backend tries[2] = {zerofg::Backend::kAuto, zerofg::Backend::kCompat};
    const int first = b->compat_only ? 1 : 0;
    int last = ZFG_ST_UNSUPPORTED;
    for (int t = first; t < 2; ++t) {
        zerofg::CreateInfo info;
        info.vulkan.instance = b->instance;
        info.vulkan.physical_device = b->pd;
        info.vulkan.device = b->dev;
        info.vulkan.get_instance_proc_addr = b->gipa;
        info.mode = b->mode == ZFG_MODE_ZERO ? zerofg::Mode::kZero : zerofg::Mode::kReallyZero;
        info.backend = tries[t];
        info.frame_context_count = b->contexts;
        zerofg::Capabilities &c = info.capabilities;
        c.effective_api_version = b->caps.api_version;
        c.synchronization2_enabled = b->caps.sync2 != 0;
        c.shader_storage_image_extended_formats_enabled = b->caps.storage_extended_formats != 0;
        c.shader_float16_supported = b->caps.fp16_supported != 0;
        c.shader_float16_enabled = b->caps.fp16_enabled != 0;
        c.subgroup_size = b->caps.subgroup_size;
        c.subgroup_min_size = b->caps.subgroup_min;
        c.subgroup_max_size = b->caps.subgroup_max;
        c.max_compute_workgroup_subgroups = b->caps.max_workgroup_subgroups;
        c.subgroup_supported_stages = b->caps.subgroup_stages;
        c.subgroup_supported_operations = b->caps.subgroup_ops;
        zerofg::Status st = zerofg::Status::kSuccess;
        std::unique_ptr<zerofg::Interpolator> fg = zerofg::Interpolator::Create(info, &st);
        if (fg) {
            st = fg->Resize(b->width, b->height, b->in_format, b->out_format);
            if (st != zerofg::Status::kSuccess) fg.reset();
        }
        last = status_of(st);
        if (fg) {
            if (status) *status = ZFG_ST_OK;
            if (backend) *backend = t == 0 ? "auto" : "compat";
            return new zfg_engine{std::move(fg)};
        }
    }
    if (status) *status = last;
    if (backend) *backend = nullptr;
    return nullptr;
}

int zfg_engine_interpolate(zfg_engine *e, VkCommandBuffer cmd, uint32_t context, const struct zfg_image *previous,
                           const struct zfg_image *current, const struct zfg_image *output) {
    if (!e || !e->fg) return ZFG_ST_INVALID;
    return status_of(e->fg->Interpolate(cmd, context, wrap(*previous), wrap(*current), 0.5f, wrap(*output)));
}

void zfg_engine_destroy(zfg_engine *e) { delete e; }

}  // extern "C"
