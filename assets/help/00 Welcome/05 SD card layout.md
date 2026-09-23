# SD Card Layout

Kefyros stores its art, help pages, settings and your files under `/kefyros` on
the SD card. The card can use FAT32 or exFAT. Extract `kefyros_sd.zip` at the
card's root so the card contains `/kefyros/icons`, `/kefyros/help` and the other
service pack folders.

## Folders

- `config.txt` - saved device and app settings. Keep this file when updating.
- `icons/` - launcher PNGs, named after their app IDs.
- `wallpapers/` - the default `wall.bin` and any images you add.
- `sfx/` - optional UI sounds.
- `help/` - Markdown help pages.
- `music/` - your MP3 library; subfolders are scanned.
- `notes/` and `chats/` - your notes and DeepSeek conversations.
- `roms/gb/` and `saves/gb/` - Game Boy ROMs and saves.
- `games/planetx3/` and `saves/planetx3/` - Planet X3 game files and saves.
- `spineko/` - browser caches created by the browser.
- `fonts/` - optional fonts.

SD-loaded KAPI apps live in `/apps/<app-id>/` at the card's root. The service
pack does not contain your music, ROMs, Planet X3 game files or saved settings.
Reapplying it updates system assets while leaving those files alone.
