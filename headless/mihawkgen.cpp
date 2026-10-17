// ProsperoEden - MihawkGen neural frame generation wrapper.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAS_MIHAWKGEN

#include "mihawkgen.h"
#include "framegen.h"
#include "diagnostics.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Eden {

MihawkGen::MihawkGen() = default;

MihawkGen::~MihawkGen() {
    Destroy();
}

bool MihawkGen::Create(const Config& config) {
    Destroy();

    Eden::Report("framegen",
                 ("MihawkGen Create: " + std::to_string(config.width) + "x" +
                  std::to_string(config.height) + " slots=" +
                  std::to_string(config.slots)).c_str());

    if (!config.gipa) {
        Eden::Report("framegen", "MihawkGen: null gipa in config");
        return false;
    }
    Eden::Report("framegen", "MihawkGen: initializing volk");
    volkInitializeCustom(config.gipa);
    volkLoadInstance(config.instance);
    volkLoadDevice(config.device);
    Eden::Report("framegen", "MihawkGen: volk ready");

    fg_create_info info{};
    info.device = config.device;
    info.physical_device = config.physical_device;
    info.width = config.width;
    info.height = config.height;
    info.slots = config.slots;
    info.output_format = config.output_format;

    char error[256] = {};
    Eden::Report("framegen", "MihawkGen: calling fg_context_create");
    VkResult result = fg_context_create(&info, &ctx_, error, sizeof(error));
    if (result != VK_SUCCESS) {
        Eden::Report("framegen",
                     ("MihawkGen: fg_context_create failed: " + std::string(error)).c_str());
        ctx_ = nullptr;
        return false;
    }
    Eden::Report("framegen", "MihawkGen: context created OK");
    return true;
}

bool MihawkGen::LoadWeights(const void* blob, size_t size) {
    if (!ctx_) {
        return false;
    }
    char error[256] = {};
    Eden::Report("framegen",
                 ("MihawkGen: loading " + std::to_string(size) + " bytes of weights").c_str());
    VkResult result = fg_context_load_network(ctx_, blob, size, error, sizeof(error));
    if (result != VK_SUCCESS) {
        Eden::Report("framegen",
                     ("MihawkGen: load_network failed: " + std::string(error)).c_str());
        network_loaded_ = false;
        return false;
    }
    Eden::Report("framegen", "MihawkGen: weights loaded OK");
    network_loaded_ = true;
    return true;
}

bool MihawkGen::LoadWeightsFromFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        Eden::Report("framegen",
                     ("MihawkGen: cannot open weights: " + std::string(path)).c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return false;
    }
    weights_data_.resize(static_cast<size_t>(len));
    size_t read = fread(weights_data_.data(), 1, weights_data_.size(), f);
    fclose(f);
    if (read != weights_data_.size()) {
        weights_data_.clear();
        return false;
    }
    bool ok = LoadWeights(weights_data_.data(), weights_data_.size());
    if (!ok) {
        weights_data_.clear();
    }
    return ok;
}

void MihawkGen::Destroy() {
    if (ctx_) {
        fg_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    network_loaded_ = false;
    frame_count_ = 0;
    prev_image_ = VK_NULL_HANDLE;
    prev_color_ = VK_NULL_HANDLE;
    curr_image_ = VK_NULL_HANDLE;
    curr_color_ = VK_NULL_HANDLE;
    weights_data_.clear();
}

bool MihawkGen::IsReady() const {
    return ctx_ && network_loaded_;
}

void MihawkGen::SubmitFrame(VkImage color_image, VkImageView color_view) {
    prev_image_ = curr_image_;
    prev_color_ = curr_color_;
    curr_image_ = color_image;
    curr_color_ = color_view;
    ++frame_count_;
}

size_t MihawkGen::GenerationCount() const {
    if (!IsReady() || frame_count_ < 2 || prev_color_ == VK_NULL_HANDLE || curr_color_ == VK_NULL_HANDLE) {
        return 0;
    }
    return 1;
}

static void RecordBarrier(VkCommandBuffer cmd, VkImage image,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void MihawkGen::RecordGenerate(VkCommandBuffer cmd, uint32_t slot, float t,
                                VkImage output_image, VkImageView output_view) {
    if (!IsReady() || prev_color_ == VK_NULL_HANDLE || curr_color_ == VK_NULL_HANDLE) {
        return;
    }

    // framegen API: inputs in SHADER_READ_ONLY_OPTIMAL, output in GENERAL
    RecordBarrier(cmd, prev_image_,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    RecordBarrier(cmd, curr_image_,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    RecordBarrier(cmd, output_image,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    fg_frame_pair pair{};
    pair.prev_color = prev_color_;
    pair.next_color = curr_color_;
    pair.prev_depth = VK_NULL_HANDLE;
    pair.next_depth = VK_NULL_HANDLE;
    pair.next_motion = VK_NULL_HANDLE;
    pair.t = t;
    fg_record_generate(ctx_, cmd, slot, FG_METHOD_NEURAL, &pair, output_view);

    // Transition output to transfer source for the present blit
    RecordBarrier(cmd, output_image,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

} // namespace Eden

#endif // HAS_MIHAWKGEN
