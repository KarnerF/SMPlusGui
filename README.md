# SMPlusGui

Web-based config UI for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) running as a PS5 payload.
Configure all SM settings from any browser, manage config backups, view the debug log, and autostart SM on launch.
Opens at **http://\<PS5-IP\>:7777** (port configurable in Startoptionen)

---

## Requirements

- Jailbroken PS5 with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) **1.6beta16 or newer** (1.7beta1 recommended)
- Payload loader (e.g. [Payload Manager](https://github.com/itsPLK/ps5-payload-manager))
- elfldr on port 9021/9020 (for Start/Autostart)

---

## Build

```bash
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

---

## Usage

1. Load `SMPlusGui_v1.0.1.elf`
2. Open `http://<PS5-IP>:7777`
3. Change settings, hit **Save**

### Auto-Update via Payload Manager

Add this URL as a Custom Source in [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) (Settings → Manage Sources → Add Source):

```
https://raw.githubusercontent.com/KarnerF/SMPlusGui/main/repo.json
```

Payload Manager will detect new SMPlusGui releases and offer the update automatically.

---

## Credits

- **[drakmor](https://github.com/drakmor)** � [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
- **[itsPLK](https://github.com/itsPLK) / [najdek](https://github.com/najdek)** � [Payload Manager](https://github.com/itsPLK/ps5-payload-manager)
- **[john-tornblom](https://github.com/john-tornblom)** � [PS5 Payload SDK](https://github.com/ps5-payload-dev)
- **[Cesanta](https://github.com/cesanta)** � [Mongoose](https://github.com/cesanta/mongoose)
- **[Lucide](https://lucide.dev)** � SVG icons
- **[GitHub Copilot](https://github.com/features/copilot)** � Built with AI assistance
- PS5 homebrew community

---

Unofficial homebrew. Use at your own risk.
