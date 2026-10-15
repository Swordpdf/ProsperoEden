// The game's own picture (hud.h's resolution piece): the size of the frame the guest handed to
// the display, with a cropped layer counted as the part that is shown, as frame generation's own
// GuestExtent counts it. Recorded before anything else, so an applet-only or skipped frame still
// says what the game last rendered.
if (!framebuffers.empty()) {
    const auto& guest_layer = framebuffers.front();
    const bool cropped = !guest_layer.crop_rect.IsEmpty();
    Eden::Hud::GuestScreen(cropped ? static_cast<u32>(guest_layer.crop_rect.GetWidth()) : guest_layer.width,
                           cropped ? static_cast<u32>(guest_layer.crop_rect.GetHeight()) : guest_layer.height);
}
