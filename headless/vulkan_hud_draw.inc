        // The overlay is laid out for a picture 1080 rows high and scaled to this one.
        const auto hud_units = [rows = render_area.height](uint32_t value) { return value * rows / 1080u; };
        if (hud.width && (hud.loading || (render_area.width >= hud_units(hud.width + 16) &&
                                          render_area.height >= hud_units(64)))) {
            const VkRect2D scissor = hud.loading ? VkRect2D{{0, 0}, render_area} :
                VkRect2D{{static_cast<int32_t>(hud_units(16)), static_cast<int32_t>(hud_units(16))},
                         {hud_units(hud.width), hud_units(48)}};
            const VkViewport viewport{static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                static_cast<float>(scissor.extent.width), static_cast<float>(scissor.extent.height), 0, 1};
            cmdbuf.SetViewport(0, viewport);
            cmdbuf.SetScissor(0, scissor);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, hud_graphics_pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, hud_pipeline_layout, 0,
                                      hud_descriptor_set, {});
            auto constants = hud;
            if (hud.loading) {
                // The loading scene fills the picture: tell it how large that is.
                constants.x = render_area.width;
                constants.y = render_area.height;
            } else {
                // Where the text starts in this picture, and the picture's height for its size.
                constants.x = hud_units(hud.x);
                constants.y = hud_units(hud.y);
                constants.width = render_area.height;
            }
            cmdbuf.PushConstants(hud_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, constants);
            cmdbuf.Draw(4, 1, 0, 0);
        }
