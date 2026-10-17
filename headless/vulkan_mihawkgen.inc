// MihawkGen neural frame generation: the Vulkan present path.
// Included into the generated vulkan_renderer.cpp by prepare-vulkan-port.py, in the
// same place as the LSFG block. Reads the bridge globals (mihawkgen_bridge.h) that
// main.cpp set before the session's renderer started.
#ifdef HAS_MIHAWKGEN
    if (Eden::MihawkGenBridge::active.load(std::memory_order_relaxed)) {
        if (!mihawkgen_init_) {
            mihawkgen_init_ = true;
            Eden::Report("framegen", "MihawkGen: init begin");
            Eden::MihawkGen::Config cfg{};
            cfg.gipa = vkGetInstanceProcAddr;
            cfg.instance = *instance;
            cfg.device = *device.GetLogical();
            cfg.physical_device = device.GetPhysical();
            cfg.width = frame->width;
            cfg.height = frame->height;
            cfg.slots = present_manager.MaxExtraFrames() + 1;
            cfg.output_format = VK_FORMAT_R8G8B8A8_UNORM;
            if (mihawkgen_.Create(cfg)) {
                if (mihawkgen_.LoadWeightsFromFile(
                        Eden::MihawkGenBridge::weights_path.c_str())) {
                    Eden::Report("framegen", "MihawkGen: ready, generating frames");
                } else {
                    Eden::Report("framegen", "MihawkGen: failed to load weights");
                }
            } else {
                Eden::Report("framegen", "MihawkGen: failed to create context");
            }
        }
        if (mihawkgen_.IsReady() && frame->image_view) {
            mihawkgen_.SubmitFrame(*frame->image, *frame->image_view);
            const size_t gen_count = mihawkgen_.GenerationCount();
            for (size_t g = 0; g < gen_count; ++g) {
                Frame* generated = present_manager.GetRenderFrame();
                blit_swapchain.PrepareFrame(device, generated,
                                            render_window.GetFramebufferLayout());
                if (generated->storage_view) {
                    const float t = 1.0f / static_cast<float>(gen_count + 1) *
                                    static_cast<float>(g + 1);
                    scheduler.RequestOutsideRenderPassOperationContext();
                    scheduler.Record(
                        [this, slot = generated->index, t,
                         out_image = *generated->image,
                         out_view = *generated->storage_view](vk::CommandBuffer cmdbuf) {
                            mihawkgen_.RecordGenerate(*cmdbuf, slot, t,
                                                      out_image, out_view);
                        });
                    scheduler.Flush(*generated->render_ready);
                    present_manager.Present(generated);
                    Eden::CountPresentedFrame();
                }
            }
        }
    }
#endif
