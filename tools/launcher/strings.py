#!/usr/bin/env python3
# ProsperoEden - The launcher's translation catalogs: template and checks.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""strings.py extract          write tools/launcher/launcher.pot from the text marked in the code
strings.py new <tag>          start headless/prosperoeden/ui/lang/<tag>.po from the template
strings.py check              check every catalog in headless/prosperoeden/ui/lang

The code holds the English text: tr("...") where it is drawn, TR("...") in constant tables (and
the setting labels of headless/settings_store.h). A catalog is a gettext .po file named after the
PS5 system language's tag (third_party/ps5_system_language.hpp): pt-BR.po, fr-FR.po...

check fails when a catalog
  - translates text the code no longer has, or leaves text untranslated,
  - changes the {0} {1} placeholders of a text,
  - uses a character the launcher's font does not have.
It warns when a translation is much longer than the English text (it may not fit its place).

The launcher's own font has Latin and Cyrillic letters. Japanese, Korean, Chinese, Greek, Thai and
Arabic are drawn with the console's fonts (pe/gfx/system_fonts.hpp): their catalogs are checked
against those when PE_SYSTEM_FONTS names a folder holding copies of them, and only for their
Latin text otherwise.
"""

import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "headless/prosperoeden"
CATALOGS = LAUNCHER / "ui/lang"
TEMPLATE = ROOT / "tools/launcher/launcher.pot"
FONT = LAUNCHER / "ui/fonts/montserrat-medium.pefont"
SETTINGS = ROOT / "headless/settings_store.h"
SETTING_LABELS = ("kResolutionLabels", "kUpscalingFilterLabels", "kAstcTextureLabels", "kLanguageLabels")

LITERALS = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
MARKED = re.compile(r"\b(?:tr|TR)\(\s*" + LITERALS)
ONE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# What a translator cannot tell from the text alone.
NOTES = {
    "Update available": "Notification title: a newer release of this app is listed (not a game update).",
    "Version {0} is on homebrew.page": "{0}: the newer release's number, for example 1.000.050; homebrew.page is a site's name and stays as it is.",
    "Version {0} is ready to install.": "Update dialog: a newer release of this app; {0} is its number, for example 1.000.060.",
    "Download size: {0}": "{0} is a size such as 36.4 MB.",
    "Your games, saves and settings are kept.": "Update dialog: installing the new app version keeps the player's files.",
    "Update now": "Button: download and install the newer release of this app now.",
    "Skip": "Button and hint: not now (the dialog shows again the next time the app opens).",
    "About {0} s left": "Time left while downloading; {0} is a number of seconds. Keep it short.",
    "About {0} min left": "Time left while downloading; {0} is a number of minutes. Keep it short.",
    "Version {0}": "{0} is the app's new version number, for example 1.000.060.",
    "Preparing": "Update dialog headline while the download starts.",
    "Unpacking": "Update dialog headline while the downloaded release is extracted.",
    "What's new": "Button on the update dialog: opens the new release's notes (what changes in it). Keep it short.",
    "What's new in version {0}": "Title of the release notes view; {0} is the new version, for example 1.000.080.",
    "Scroll": "Button hint: move through a long text (Up/Down).",
    "The rest is on the app's page on homebrew.page.": "Shown at the end of release notes that were cut short; homebrew.page is a site's name and stays as it is.",
    "Select": "Button hint: choose the highlighted item (not the Select button).",
    "Back": "Button hint: go back one screen.",
    "Change": "Button hint: change the highlighted setting.",
    "Open": "Button hint: open the highlighted folder.",
    "Browse": "Button hint: move through the list.",
    "Page": "Button hint: jump a page of the list (L1 / R1).",
    "Details": "Button hint: show the game's details.",
    "Navigate": "Button hint: move the highlight.",
    "Choose": "Button hint: choose the highlighted language.",
    "UP": "Short tag on the 'Parent folder' row.",
    "OPEN": "Short tag on a folder row.",
    "IN USE": "Tag on the language or folder that is in use now.",
    "Docked": "Console mode: the console as if connected to a TV.",
    "Handheld": "Console mode: the console as if held in the hands.",
    "{0} OF {1}": "Position in a list: 3 OF 12.",
    "Off": "A setting that is switched off.",
    "On": "A setting that is switched on.",
    "None": "No add-ons (updates or DLC).",
    "Default ({0})": "A per-game setting that follows the general setting; {0} is its value.",
    "{0} not available": "{0} is a language name. Shown beside a game that lacks that language.",
    "Add-ons: {0}  /  Language: {1} ({2} in this game)": "{2} is the text '{0} not available'.",
    "STARTING": "Shown over a game's cover while it starts.",
    "Make it yours.": "Headline of the Settings screen.",
    "Your next adventure": "Headline when no game has been played yet.",
    "{0}/ (NSP or XCI)": "{0} is a folder; NSP and XCI are file types.",
    "Docked": "Console mode: the console as if connected to a TV. Keep it short (about 9 letters).",
    "Handheld": "Console mode: the console as if held in the hands. Keep it short (about 9 letters).",
    "Update {0}": "A game update; {0} is its version, for example 1.2.0.",
    "{0} DLC": "{0} is how many DLC (add-on content) a game has installed.",
    "{0} game": "Exactly one game. In a language with more than two plural forms, word it so that "
                "it reads well with any number (Games: {0}).",
    "{0} games": "Any number of games other than one (also 0). See '{0} game'.",
    "{0} game installed": "Exactly one game. See '{0} game'.",
    "{0} games installed": "Any number of games other than one. See '{0} game'.",
    "{0} NCA file": "Exactly one file. See '{0} game'.",
    "{0} NCA files": "Any number of files other than one. See '{0} game'.",
    "Keep keys, firmware and roms folders together. TRIANGLE uses the folder shown.":
        "keys, firmware and roms are folder names (unchanged). TRIANGLE is the controller's "
        "triangle button, in capitals.",
    "Select is the touchpad button on PS5.": "Select is a button's name (unchanged).",
    "Nearest": "An upscaling filter (nearest neighbour). Keep it short.",
    "Bilinear": "An upscaling filter.",
    "Bicubic": "An upscaling filter.",
    "ASTC textures": "Setting: how a game's textures in the ASTC format are held in graphics memory. ASTC "
                     "is a format's name (unchanged).",
    "ASTC TEXTURES": "Label: see 'ASTC textures'.",
    "Compressed": "A choice of 'ASTC textures': the textures are kept compressed (less memory). Keep it short.",
    "Full quality": "A choice of 'ASTC textures': the textures are kept uncompressed (more memory). Keep it "
                    "short.",
    "Saved. Full quality uses four times the graphics memory for these textures and can run out above 1x.":
        "Shown after choosing 'Full quality' for 'ASTC textures'. 1x is a resolution scale (unchanged). "
        "It has two lines: keep it short.",
    "Vulkan (recommended)": "Vulkan is a name (unchanged).",
    "Game could not start: {0} Details: {1}": "{0} is the reason, a sentence in English; {1} is a file.",
    "Mods": "Changes to a game that the player added: patches, replaced files, cheats. Keep the word "
            "'mod' if the language uses it.",
    "No mods": "Shown on the Mods row of a game that has none.",
    "{0} of {1} on": "How many of a game's mods are switched on: 2 of 3 on.",
    "{0} mod": "Exactly one mod, in the list of what a game comes with (Update 1.2.0, 2 DLC, 1 mod). "
               "See '{0} game'.",
    "{0} mods": "Any number of mods other than one. See '{0} mod'.",
    "{0} of {1} mods on": "In the same list, when some of the game's mods are switched off: "
                          "1 of 2 mods on. Keep it short.",
    "Patch": "What a mod is made of: a change to the game's program (not a game update).",
    "Files": "What a mod is made of: files that replace the game's own.",
    "Cheats": "What a mod is made of: cheat codes.",
    "No mods for this game yet. Copy each mod's folder to {0}, next to roms/.":
        "{0} is a folder; roms/ is a folder name (unchanged).",
    "Turn on or off": "Button hint: switch the highlighted mod on or off.",
    "Create the folder": "Button hint: make the folder a game's mods go in.",
    "Created {0}. Copy each mod's folder into it.": "{0} is a folder.",
    "ProsperoEden stopped because of an error. A report was saved to {0}.":
        "Shown on the home screen after the app crashed and started again; {0} is a file.",
    "Sandboxed (code {0}): app folder only": "The app can read only its own folder; {0} is a number.",
    "Full filesystem": "The app can read every folder of the console.",
    "END GAME": "Label of the shortcut that ends the running game.",
    "FPS OVERLAY": "Label: the frames-per-second counter drawn over a game.",
    "SETUP": "Label: whether keys and firmware are in place.",
    "ACCESS": "Label: which folders the app can read.",
    "KEYS": "Label: the encryption keys file (prod.keys).",
    "Refresh rate": "Setting: how many times a second the TV picture is refreshed while a game runs "
                    "(60 or 120 Hz).",
    "REFRESH RATE": "Label: see 'Refresh rate'.",
    "{0} Hz": "{0} is 60 or 120 (hertz).",
    "Saved. A display that cannot show 120 Hz stays at 60 Hz.": "Shown after choosing 120 Hz.",
    "MODS": "Label on the About screen: the folder that holds games' mods (see 'Mods').",
    "{0}/ (one folder per game ID)": "{0} is the mods folder; inside it each game has a folder named after "
                                     "its ID (16 letters and digits).",
    "Output resolution": "Setting: the size of the picture sent to the TV (1080p, 1440p or 2160p). Not the "
                         "same as 'Resolution', which scales the game's own picture (1x, 2x...).",
    "OUTPUT RESOLUTION": "Label: see 'Output resolution'.",
    "ADD-ONS": "Label: a game's updates, DLC and mods.",
    "Add-ons: {0}  /  Language: {1}": "{0}: updates, DLC and mods of the game; {1}: the language it will use.",
    "Ryujinx save": "A save file of the Ryujinx emulator. Ryujinx is a name (unchanged).",
    "Last game opened": "Caption under the title of the game played last.",
    "Powered by Eden": "Eden is the emulator's name (unchanged).",
    "THANKS": "Heading of the acknowledgements.",
    "SELECTED": "Heading: the language highlighted in the list.",
    "NEXT START": "Label: the folder used the next time the app starts.",
    "Next launch: {0}": "{0} is the folder used the next time the app starts.",
    "Accessibility": "A settings category: options that make the menu easier to see and follow.",
    "Larger text": "A switch: the menu's small text is drawn larger.",
    "High contrast": "A switch: solid dark panels, brighter text.",
    "Reduce motion": "A switch: no drifting, sliding or zooming on screen.",
    "Save data": "A row of a game's settings: the game's saved progress, which can be copied in or out.",
    "Ryujinx save found": "Short status at the right of the Save data row. Ryujinx is a name (unchanged).",
    "Save folder found": "Short status at the right of the Save data row: a folder with a save to import.",
    "Nothing to import": "Short status at the right of the Save data row.",
    "Import": "Button hint: copy a save in.",
    "Export a copy": "Button hint: copy the game's save out to a folder.",
    "Press again to replace this game's save. The current one is backed up.":
        "Asked before a save is imported over the one in use.",
    "To import, copy a Ryujinx folder to ryujinx/ or a save to save-import/{0}/, next to roms/.":
        "ryujinx/, save-import/ and roms/ are folder names (unchanged); {0} is the game's ID.",
    "Exported to {0}.": "{0} is a folder.",
    "Start any game once before importing a save.": "The app creates its user the first time a game runs.",
    "Selected ROM is no longer available": "Why a game did not start (the file is gone).",
    "PS5 controller initialization failed": "Why a game did not start.",
    "Performance": "A settings category: options that make games run faster, at some cost in accuracy.",
    "System": "A category of a game's settings: how the emulated console itself behaves.",
    "RNG seed": "A game setting. RNG is a random number generator and the seed is the number it starts "
                "from. A game with a seed is loaded at the same place in memory at every start, which some "
                "mods need. Keep it short; RNG and seed may stay in English where players say them so.",
    "Random": "The usual value of RNG seed: no fixed seed, a new random one at every start of the game.",
    "Edit": "A button hint: opens the keyboard to type a number. One short word, and not the word used "
            "for Change.",
    "Enter a number between 0 and 4294967295.": "Shown when what was typed for the RNG seed is not such "
                                                "a number.",
    "Compile ahead": "A switch: the program code a game used in earlier sessions is prepared (compiled) "
                     "while the game starts. Keep it short.",
    "Asynchronous shaders": "A switch. A shader is a small graphics program; keep the word the language's "
                            "players use for it.",
    "Faster GPU emulation": "A switch: the emulated graphics processor is less exact and faster. GPU stays.",
    "Faster CPU emulation": "A switch: the emulated processor's floating-point math is less exact and faster. "
                            "CPU stays.",
    "Faster DMA": "A switch: memory transfers to the emulated graphics processor are less exact and faster. "
                  "DMA is a name (unchanged).",
    "Reactive flushing": "A switch, on by default: what a game reads back from the graphics processor is kept "
                         "exact. Keep the term the language's emulator players use, or translate it plainly.",
    "Skip CPU invalidation": "A switch: fewer checks when a game changes memory the graphics processor uses. "
                             "CPU stays.",
    "Touchpad": "The DualSense controller's touch pad, pressed as a button. Use the name players know.",
    "Button mapping": "Which controller button presses each of the game's buttons.",
    "As usual": "The button mapping has not been changed.",
    "Changed": "The button mapping has been changed.",
    "Cross": "A DualSense button (the X-shaped one). Name the shape, not the letter.",
    "Circle": "A DualSense button.",
    "Square": "A DualSense button.",
    "Triangle": "A DualSense button.",
    "Options": "The DualSense button labelled OPTIONS: keep the label if the language's players do.",
    "Create": "The DualSense button labelled CREATE: keep the label if the language's players do.",
    "Left stick press": "Pressing the left stick down like a button.",
    "Follows Settings": "A kind of setting that this game takes from the launcher's Settings menu.",
    "{0} changed": "How many settings of a kind this game has of its own; {0} is a number.",
    "This game": "The game has a button mapping of its own.",
    "{0}%": "A percentage (a volume): write it as the language does, for example with a space before the sign.",
    "The game ran out of graphics memory. Lower the resolution in Settings, Video (or in the game's own settings) "
    "and start it again.": "Why a game stopped. 'Settings, Video' is the menu path; 'the game's own settings' is the "
                           "Game settings dialog of the Library.",
    "Play": "Button and button hint: start the game shown. Keep it to one short word.",
    "Sections": "Button hint beside L1 and R1: they move between the menu's sections (Home, Library, Multiplayer, Settings, About).",
    "Home": "The menu's first section, its opening screen (not a house, and not the PS5's own home screen).",
    "Theme": "A tile of Settings and its dialog's title: the look of this menu (its colours and artwork).",
    "The look of this menu.": "Under the title Theme: what a theme changes.",
    "Tropical dusk, dark green glass and lime.": "One line about the theme named Canopy: its artwork is a tropical sunset, its panels are dark green glass, its accent colour is lime green.",
    "About": "The menu's last section: about this app (its credits, where its files go). One short word.",
}


def unescape(text):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), text)


def escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def joined(literals):
    return "".join(unescape(part) for part in ONE.findall(literals))


def marked_text():
    """English text -> the files that use it."""
    found = {}
    sources = sorted(p for pattern in ("*.cpp", "*.hpp", "*.h") for p in LAUNCHER.rglob(pattern))
    for path in sources:
        source = path.read_text(encoding="utf-8")
        # Comments may quote tr("...") too; drop them.
        source = re.sub(r"//[^\n]*", "", source)
        for match in MARKED.finditer(source):
            text = joined(match.group(1))
            if text:
                found.setdefault(text, []).append(path.relative_to(LAUNCHER).as_posix())
    settings = SETTINGS.read_text(encoding="utf-8")
    for name in SETTING_LABELS:
        table = re.search(name + r"\[\] = \{([^}]*)\}", settings)
        assert table, name
        for text in ONE.findall(table.group(1)):
            found.setdefault(unescape(text), []).append("settings_store.h")
    return found


def parse_po(path):
    """msgid -> msgstr of a catalog (the header entry is skipped)."""
    entries, key, value, part = {}, None, None, None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("msgid "):
            if key:
                entries[key] = value or ""
            key, value, part = joined(line[6:]), "", "id"
        elif line.startswith("msgstr "):
            value, part = joined(line[7:]), "str"
        elif line.startswith('"'):
            if part == "id":
                key += joined(line)
            elif part == "str":
                value += joined(line)
    if key:
        entries[key] = value or ""
    return entries


def write_catalog(path, texts, translations, language):
    lines = [f"# ProsperoEden launcher - {language}",
             "# English text is the key (msgid); msgstr is the translation. Keep {0} {1} as they are,",
             "# keep UPPERCASE labels uppercase, and keep names (ProsperoEden, Eden, PS5, Vulkan, OpenGL,",
             "# AMD FSR, DLC, NSP, XCI, prod.keys, Ryujinx) unchanged.",
             ""]
    for text in sorted(texts, key=str.lower):
        if text in NOTES:
            lines.append(f"#. {NOTES[text]}")
        lines.append("#: " + ", ".join(sorted(set(texts[text]))))
        lines.append(f'msgid "{escape(text)}"')
        lines.append(f'msgstr "{escape(translations.get(text, ""))}"')
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def font_characters():
    data = FONT.read_bytes()
    magic, _version, _w, _h, _size, _range, _asc, _desc, _gap, glyphs, _kerns = struct.unpack_from("<IIHHfffffII", data)
    assert magic == 0x46505A50, "not a .pefont file"
    return {struct.unpack_from("<I", data, 40 + 24 * index)[0] for index in range(glyphs)}


# Catalogs written in scripts the console's fonts draw.
SYSTEM_FONT_CATALOGS = {"ja-JP", "ko-KR", "zh-Hans", "zh-Hant", "el-GR", "th-TH", "ar"}
# Characters that take no room: the zero-width space (a line may break there), direction marks.
INVISIBLE = {0x200B, 0x200C, 0x200D, 0x200E, 0x200F}


def cmap_characters(data):
    """The characters a TrueType or OpenType font file maps (its Unicode cmap, format 4 or 12)."""
    offset = struct.unpack_from(">I", data, 12)[0] if data[:4] == b"ttcf" else 0
    tables = {}
    for index in range(struct.unpack_from(">H", data, offset + 4)[0]):
        tag, _sum, start, _length = struct.unpack_from(">4sIII", data, offset + 12 + 16 * index)
        tables[tag] = start
    cmap = tables[b"cmap"]
    best = None
    for index in range(struct.unpack_from(">H", data, cmap + 2)[0]):
        platform, encoding, at = struct.unpack_from(">HHI", data, cmap + 4 + 8 * index)
        kind = struct.unpack_from(">H", data, cmap + at)[0]
        rank = {(3, 10): 4, (0, 4): 4, (0, 6): 4, (3, 1): 2, (0, 3): 2}.get((platform, encoding), 0)
        if kind in (4, 12) and (best is None or rank > best[0]):
            best = (rank, kind, cmap + at)
    characters = set()
    if best is None:
        return characters
    _rank, kind, at = best
    if kind == 12:
        for index in range(struct.unpack_from(">I", data, at + 12)[0]):
            first, last, _glyph = struct.unpack_from(">III", data, at + 16 + 12 * index)
            characters.update(range(first, last + 1))
        return characters
    segments = struct.unpack_from(">H", data, at + 6)[0] // 2
    ends = struct.unpack_from(f">{segments}H", data, at + 14)
    starts = struct.unpack_from(f">{segments}H", data, at + 16 + 2 * segments)
    deltas = struct.unpack_from(f">{segments}h", data, at + 16 + 4 * segments)
    ranges_at = at + 16 + 6 * segments
    ranges = struct.unpack_from(f">{segments}H", data, ranges_at)
    for i in range(segments):
        for code in range(starts[i], min(ends[i], 0xFFFE) + 1):
            if ranges[i] == 0:
                glyph = (code + deltas[i]) & 0xFFFF
            else:
                glyph = struct.unpack_from(">H", data, ranges_at + 2 * i + ranges[i] + 2 * (code - starts[i]))[0]
            if glyph:
                characters.add(code)
    return characters


def system_font_characters():
    """The characters of the console's fonts in PE_SYSTEM_FONTS, or None when it is not set."""
    folder = os.environ.get("PE_SYSTEM_FONTS")
    if not folder:
        return None
    characters = set()
    for path in sorted(Path(folder).iterdir()):
        if path.suffix.lower() in (".otf", ".ttf"):
            characters |= cmap_characters(path.read_bytes())
    return characters


def launch_error_problems():
    """The launch errors the launcher translates must still be what headless/main.cpp reports."""
    services = (LAUNCHER / "eden_services.cpp").read_text(encoding="utf-8")
    table = re.search(r"kLaunchErrors\[\] = \{(.*?)\};", services, re.S)
    if not table:
        return ["eden_services.cpp has no kLaunchErrors table"]
    reported = (ROOT / "headless/main.cpp").read_text(encoding="utf-8")
    reported = re.sub(r'"\s*\n\s*"', "", reported)  # a sentence written as adjacent literals
    return [f"main.cpp no longer reports: {joined(literal)!r}"
            for literal in re.findall(r"TR\(\s*" + LITERALS, table.group(1))
            if '"' + escape(joined(literal)) + '"' not in reported]


def check():
    texts = marked_text()
    baked = font_characters()
    system = system_font_characters()
    failed = False
    for problem in launch_error_problems():
        print(problem)
        failed = True
    catalogs = sorted(CATALOGS.glob("*.po"))
    if not catalogs:
        print("no catalogs in", CATALOGS)
    for path in catalogs:
        entries = parse_po(path)
        problems, warnings = [], []
        # What can be drawn: the launcher's font, and for the catalogs of other scripts the
        # console's fonts (when they are at hand; otherwise only their Latin text is checked).
        characters = baked | INVISIBLE
        unchecked = set()
        if path.stem in SYSTEM_FONT_CATALOGS:
            if system is None:
                unchecked = {ord(c) for text in entries.values() for c in text if ord(c) not in characters and c != "\n"}
                characters = characters | unchecked
            else:
                characters = characters | system
        for text in texts:
            if not entries.get(text):
                problems.append(f"untranslated: {text!r}")
        for text, translation in entries.items():
            if text not in texts:
                problems.append(f"not in the code any more: {text!r}")
                continue
            if sorted(re.findall(r"\{\d\}", text)) != sorted(re.findall(r"\{\d\}", translation)) and translation:
                problems.append(f"placeholders differ: {text!r} -> {translation!r}")
            missing = sorted({c for c in translation if ord(c) not in characters and c not in "\n"})
            if missing:
                problems.append(f"characters the font lacks {''.join(missing)!r} in {translation!r}")
            if text.isupper() and translation and translation != translation.upper():
                warnings.append(f"label not uppercase: {text!r} -> {translation!r}")
            if translation and len(translation) > max(len(text) * 1.7, len(text) + 12):
                warnings.append(f"long ({len(text)} -> {len(translation)}): {translation!r}")
        note = f", {len(unchecked)} characters of the console's fonts not checked" if unchecked else ""
        print(f"{path.name}: {len(entries)} texts, {len(problems)} problems, {len(warnings)} warnings{note}")
        for line in problems[:40]:
            print("   ", line)
        for line in warnings[:12]:
            print("    warning:", line)
        failed |= bool(problems)
    print(f"{len(texts)} texts in the code, {len(catalogs)} catalogs" + (" FAIL" if failed else " PASS"))
    return 1 if failed else 0


def main():
    # Translations are printed as they are, whatever the console's own encoding.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "extract":
        texts = marked_text()
        write_catalog(TEMPLATE, texts, {}, "template")
        print(f"{TEMPLATE.relative_to(ROOT)}: {len(texts)} texts, {sum(len(t.split()) for t in texts)} words")
    elif command == "new" and len(sys.argv) == 3:
        path = CATALOGS / f"{sys.argv[2]}.po"
        existing = parse_po(path) if path.exists() else {}
        write_catalog(path, marked_text(), existing, sys.argv[2])
        print(f"{path.relative_to(ROOT)}: {len(existing)} translations kept")
    elif command == "check":
        sys.exit(check())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
