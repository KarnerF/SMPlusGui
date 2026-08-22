# SMPlusGui

A fun side project — a web-based config UI for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) that runs as a PS5 payload. Built for personal use with AI assistance.

Opens at **http://\<PS5-IP\>:7070** from any browser.

- Configure all ShadowMountPlus settings without FTP or text editor
- Autostart SM on launch, config backups, live debug log, Start/Stop via elfldr
- Recovers after PS5 rest mode
- DE/EN/FR/ES — auto-detected from PS5 system language, others fall back to English

---

## Requirements

- Jailbroken PS5
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
- A payload loader (e.g. [Payload Manager](https://github.com/itsPLK/ps5-payload-manager))
- elfldr on port 9021/9020 (for SM Start and Autostart)

---

## Build

```bash
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

---

## Usage

1. Load `SMPlusGui_1.0.0.elf`
2. Open `http://<PS5-IP>:7070`
3. Configure settings and hit **Save**

App settings (autostart, icon) are stored in `/data/SMPlusGui/prefs.ini`.

---

## Credits

- **[drakmor](https://github.com/drakmor)** — [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
- **[itsPLK](https://github.com/itsPLK) / [najdek](https://github.com/najdek)** — [PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager)
- **[john-tornblom](https://github.com/john-tornblom)** — [PS5 Payload SDK](https://github.com/ps5-payload-dev)
- **[Cesanta](https://github.com/cesanta)** — [Mongoose](https://github.com/cesanta/mongoose)
- **[Lucide](https://lucide.dev)** — SVG icons
- **[GitHub Copilot](https://github.com/features/copilot)** — Built with AI assistance (GitHub Copilot / Claude Sonnet)
- PS5 homebrew community

---

## Disclaimer

Unofficial homebrew. Use at your own risk. Not affiliated with Sony.


---

## Features

- **Full ShadowMountPlus configuration** — all settings from one clean web interface
- **Autostart** — automatically launches ShadowMountPlus when the GUI starts; configurable ELF path, extra scan directories, and restart behavior
- **Backup management** — create, restore, download, copy and move backups between internal storage and USB; named backups with timestamps
- **Raw config editor** — directly edit `config.ini` with search function
- **Debug log viewer** — live log with category filters and clear function
- **SM Start/Stop** — launch ShadowMountPlus via elfldr (port 9021/9020) directly from the GUI
- **Rest mode recovery** — automatically restores the web server after PS5 wakes from standby
- **Multi-language UI** — German, English, French, Spanish + automatic PS5 system language detection
- **USB detection** — automatically refreshes when a USB drive is plugged or unplugged
- **Collapsible sidebar** — toggle menu with the ☰ button
- **Responsive design** — works on PS5 browser, mobile phones, and desktop
- **PS5 home screen tile** — installs a launcher icon (SMPL00001); configurable to always move to front or install once

---

## Requirements

- PS5 with a homebrew environment (e.g. via BD-JB or Y2JB)
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) installed
- A payload loader such as [Payload Manager](https://github.com/itsPLK/ps5-payload-manager)
- elfldr running on port 9021 or 9020 (for SM Start button and Autostart)

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
3. Configure ShadowMountPlus settings and press **Save**
4. Use the **Autostart** panel to automatically launch SM on GUI startup
5. Use the **Config** panel to manage configuration backups

---

## Autostart

The Autostart panel lets you configure automatic SM startup:

- **Auto-Start toggle** — enable/disable automatic launch on GUI start
- **Preferred ELF** — select or type the path to your SM ELF (uses ELF picker by default)
- **Additional scan directories** — extra directories scanned for SM ELFs in addition to the defaults
- **Always restart SM** — when off (default), SM is only launched if not already running; when on, always restarts
- **Icon position** — toggle whether the home screen icon moves to front on every launch

Default scan locations: `/data/pldmgr/payloads/*/`, `/data/shadowmount/`, `/mnt/usb0-7/`

App preferences (autostart, icon settings) are stored in `/data/SMPlusGui/prefs.ini`.

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

- **[itsPLK](https://github.com/itsPLK) / [najdek](https://github.com/najdek)** — [PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager), which served as architectural reference and recommended payload loader.

- **[john-tornblom](https://github.com/john-tornblom)** — [PS5 Payload SDK](https://github.com/ps5-payload-dev) and various reference payloads that made this project possible.

- **[Cesanta](https://github.com/cesanta)** — [Mongoose](https://github.com/cesanta/mongoose) embedded HTTP server, used as the networking backbone.

- **[Lucide](https://lucide.dev)** — Open-source icon library (MIT license), providing the clean SVG icons used throughout the UI.

- **The PS5 homebrew community** — for continued research, testing, and support.

- **[GitHub Copilot](https://github.com/features/copilot)** — This project was built with AI assistance (GitHub Copilot / Claude Sonnet).

---

## Disclaimer

This is unofficial homebrew software. Use at your own risk.  
Not affiliated with Sony Interactive Entertainment or any game publisher.

App data is stored under `/data/SMPlusGui/` on the PS5 internal storage.  
ShadowMountPlus config is stored at `/data/shadowmount/config.ini`.  
Always keep backups of important configurations before making changes.
