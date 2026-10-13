# Roadmap

- **FPKG support** - install ProsperoEden as a fake package, alongside the current homebrew folder install.
- **More performance** - CPU and GPU work to keep demanding games at their target frame rate: the short stutter when a game reaches new areas (the block list, **Compile ahead** in **Settings > Performance**, once it has run in more games and can be on by default), heavy cutscenes, and games that run slower in Docked mode than in Handheld.
- **More reliable game loading** - a game that hangs while it loads can now be left with Touchpad + L1; the hangs themselves still need a log from a game that does it.
- **Game names in the menu's language** - a game carries a name for each language; the Library shows the first one that is text. Show the one in the menu's language, and the name from an installed update when it has one.
- **Real Joy-Cons and Pro Controllers (investigation)** - play with the original controllers connected to the PS5. The PS5 does not pair them itself, so this first needs to find out whether a homebrew app can read them: a wired Pro Controller over USB looks more likely than Joy-Cons, which only connect over Bluetooth.
- **Keyboard as a controller** - map a USB keyboard's keys to the game's buttons, like the DualSense's. Text entry itself is done: it uses the PS5's on-screen keyboard.
- **Multiplayer with Eden on other systems (investigation)** - join Eden's multiplayer rooms from the PS5 and play with Eden players on PC, Linux and Android, entering a room's address by hand, including a room hosted on your local network (no PSN needed). Rooms only accept the same Eden version on every side, so each release would name the matching PC version.
- **DualShock 4 controllers** - use DualShock 4 controllers as players too, next to the DualSense, so multiplayer games do not need four DualSense controllers.
- **Controller type for each player** - choose, for each of the four connected controllers, which controller the game sees: a Pro Controller, a pair of Joy-Cons, a single left or right Joy-Con, a GameCube controller, or Handheld mode, as Eden does on other systems.
- **Better OpenGL performance** - make the OpenGL renderer faster, and add tuning options for it.
- **More game compatibility** - validate more games on the PS5, and fix what keeps them from running well, such as games that crash at launch.
