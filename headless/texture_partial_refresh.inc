        // headless/texture_partial.h: an image that stayed Tracked uploads the rows of its written pages.
        auto& partial = Eden::TexturePartial::images[image_id.index];
        if (const auto pages = Eden::TexturePartial::End(device_memory, image_id.index); !pages.empty()) {
            image.flags &= ~ImageFlagBits::CpuModified;
            const auto ranges = Eden::TexturePartial::Ranges(pages, image.cpu_addr, image.guest_size_bytes);
            const auto read = [this, &image](u32 offset, u8* data, u32 size) {
                gpu_memory->ReadBlockUnsafe(image.gpu_addr + offset, data, size);
            };
            if (const auto copies = UnswizzleImageRows(image.info, static_cast<u32>(image.guest_size_bytes),
                                                       ranges, read, unswizzle_data_buffer)) {
                size_t bytes = 0;
                for (const auto& copy : *copies) bytes += copy.buffer_size;
                if (Eden::Performance::tex_partial_check.load(std::memory_order_relaxed) && bytes != 0) {
                    swizzle_data_buffer.resize_destructive(image.guest_size_bytes);
                    gpu_memory->ReadBlockUnsafe(image.gpu_addr, swizzle_data_buffer.data(), image.guest_size_bytes);
                    const bool same = CheckImageRows(*gpu_memory, image.gpu_addr, image.info,
                        static_cast<u32>(image.guest_size_bytes),
                        std::span<const u8>(swizzle_data_buffer.data(), image.guest_size_bytes), *copies,
                        std::span<const u8>(unswizzle_data_buffer.data(), bytes));
                    Eden::Performance::tex_partial_checks.fetch_add(1, std::memory_order_relaxed);
                    if (!same) Eden::Performance::tex_partial_mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                if (bytes != 0) {
                    auto staging = runtime.UploadStagingBuffer(bytes);
                    std::memcpy(staging.mapped_span.data(), unswizzle_data_buffer.data(), bytes);
                    image.UploadMemory(staging, *copies);
                    runtime.InsertUploadMemoryBarrier();
                }
                Eden::Performance::tex_partial_uploads.fetch_add(1, std::memory_order_relaxed);
                Eden::Performance::tex_partial_bytes.fetch_add(bytes, std::memory_order_relaxed);
                return;
            }
            // A layout the rows cannot follow: the whole upload below, which tracks the image again.
            UntrackImage(image, image_id);
            image.flags |= ImageFlagBits::CpuModified;
        }
        partial.uploaded = true;
