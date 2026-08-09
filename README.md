# SMPlusGui

A web-based configuration UI for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) running directly on the PS5 as a payload ELF.

Opens at **http://\<PS5-IP\>:7070** from any browser — PS5, phone, laptop.

---

## Features

- **Full ShadowMountPlus configuration** — all settings from one clean web interface
- **Backup management** — create, restore, download, copy and move backups between internal storage and USB
- **Multi-language UI** — German, English, French, Spanish + automatic PS5 system language detection
- **USB detection** — automatically refreshes when a USB drive is plugged or unplugged
- **Debug log viewer** — live log with category filters and clear function
- **Responsive design** — works on PS5 browser, mobile phones, and desktop
- **PS5 home screen tile** — installs a launcher icon for quick access (PPSX99998)

---

## Requirements

- PS5 with a homebrew environment (e.g. via BD-JB or Y2JB)
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) installed and running
- A payload loader such as [Payload Manager](https://github.com/itsPLK/ps5-payload-manager)

---

## Build

```bash
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

Output: `SMPlusGui_1.0.0.elf`

---

## Usage

1. Load `SMPlusGui_1.0.0.elf` via your payload loader
2. Open `http://<PS5-IP>:7070` in any browser
3. Configure ShadowMountPlus settings and press **Speichern**
4. Use the **Backup** panel to manage configuration backups

The PS5 home screen tile is reinstalled on every launch to keep it at the front.

---

## Backup System

Backups are stored in `/data/SMPlusGui/backups/` on the PS5.  
USB backups go to `<USB>/SMPlusGui/` on the connected drive.

Per-backup operations (copy/move/restore/download) are available for both internal and USB backups.  
Bulk operations (copy all, move all, download all, delete all) are available in the section footer.

---

## Credits & Thanks

This project would not exist without the work of the following people:

- **[drakmor](https://github.com/drakmor)** — Creator of [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), the mount manager this GUI is built for.

- **[itsPLK](https://github.com/itsPLK) / [najdek](https://github.com/najdek)** — [PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager), which serves as the recommended payload loader for this ELF.

- **[john-tornblom](https://github.com/john-tornblom)** — [PS5 Payload SDK](https://github.com/ps5-payload-dev) and various reference payloads that made this project possible.

- **[Cesanta](https://github.com/cesanta)** — [Mongoose](https://github.com/cesanta/mongoose) embedded HTTP server, used as the networking backbone.

- **[Lucide](https://lucide.dev)** — Open-source icon library (MIT license), providing the clean SVG icons used throughout the UI.

- **The PS5 homebrew community** — for continued research, testing, and support.

---

## Disclaimer

This is unofficial homebrew software. Use at your own risk.  
Not affiliated with Sony Interactive Entertainment or any game publisher.

Config files are stored on the PS5 internal storage under `/data/SMPlusGui/`.  
Always keep backups of important configurations before making changes.
