# Wallpapers

An image of your own behind the launcher's screens. **Settings > Appearance > Wallpaper**
lists the images in the wallpapers folder, with a switch that blurs the picture itself. The
choice is kept in the settings file as `"launcher": { "wallpaper": "beach.png",
"wallpaper_blur": false }`; with no wallpaper (or one this build cannot read) the theme's own
art shows, as before.

## The folder

The images are read from the wallpapers folder on the console:

```
/data/prosperoeden/wallpapers
```

The app creates the folder when it starts. Put images in it over FTP while the app is
running (the dialog shows the same path under its list), then leave and reopen **Settings >
Appearance > Wallpaper**: the list is read each time the dialog opens. An
image already in use is loaded again when the settings are saved, so a replaced file takes
effect without restarting.

## Formats

PNG and JPEG are what a photo or a screenshot usually is; BMP, GIF (its first picture), TGA
and BC7 DDS are read too. An image over 8192 pixels a side is halved until it fits, and the
whole picture is resized to the screen: an image of another shape keeps its own proportions,
with the theme showing behind what it leaves over, instead of being stretched or cut.

## Blur image

The first row of the dialog, **Blur image**, shows the wallpaper itself blurred — the
frosted panels blur their part of it either way. The switch stays in the dialog, so the
picture can be compared both ways before leaving; it applies at once, as the picture does.

## What shows where

The Home screen and the Library draw one background image, in this order:

1. a game's own cover, where the theme shows one (Home and the selected game in the Library);
2. the wallpaper, when one is chosen;
3. the theme's own artwork.

Panels and cards sample the blurred copy of whichever of them is in use.
