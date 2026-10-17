// ProsperoEden - MihawkGen neural frame generation wrapper.
// Adapts PS5_MihawkGen's C API (framegen.h) to Eden's Vulkan present path.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HAS_MIHAWKGEN

#include <cstddef>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

struct fg_context;

namespace Eden {

class MihawkGen {
public:
    struct Config {
        PFN_vkGetInstanceProcAddr gipa;
        VkInstance instance;
        VkDevice device;
        VkPhysicalDevice physical_device;
        uint32_t width;
        uint32_t height;
        uint32_t slots;
        VkFormat output_format;
    };

    MihawkGen();
    ~MihawkGen();

    MihawkGen(const MihawkGen&) = delete;
    MihawkGen& operator=(const MihawkGen&) = delete;

    bool Create(const Config& config);
    bool LoadWeights(const void* blob, size_t size);
    bool LoadWeightsFromFile(const char* path);
    void Destroy();

    bool IsReady() const;

    void SubmitFrame(VkImage color_image, VkImageView color_view);

    size_t GenerationCount() const;

    // Records barriers + compute dispatches for one generated frame. The caller
    // must NOT have transitioned the inputs or output — this method handles all
    // layout transitions required by the framegen API.
    void RecordGenerate(VkCommandBuffer cmd, uint32_t slot, float t,
                        VkImage output_image, VkImageView output_view);

private:
    fg_context* ctx_ = nullptr;
    bool network_loaded_ = false;
    uint64_t frame_count_ = 0;
    VkImage prev_image_ = VK_NULL_HANDLE;
    VkImageView prev_color_ = VK_NULL_HANDLE;
    VkImage curr_image_ = VK_NULL_HANDLE;
    VkImageView curr_color_ = VK_NULL_HANDLE;
    std::vector<uint8_t> weights_data_;
};

} // namespace Eden

#endif // HAS_MIHAWKGEN
