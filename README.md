# SMPlusGui

A fun side project � web-based config UI for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) running as a PS5 payload. Built with AI assistance.
Configure all SM settings from any browser, manage config backups, view the debug log, and autostart SM on launch.
Opens at **http://\<PS5-IP\>:7070**

---

## Requirements

- Jailbroken PS5 with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
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

1. Load `SMPlusGui_1.0.0.elf`
2. Open `http://<PS5-IP>:7070`
3. Change settings, hit **Save**

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
