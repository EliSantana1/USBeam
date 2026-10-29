# USBeam

**Free, open-source USB over IP for Windows and Linux.**

Plug a USB device into one computer and use it on another over your network,
as if it were plugged in there.

## What it does

- **Share and use devices.** Every installation can share its own USB devices
  and use devices shared by other computers. No device limits.
- **Finds your computers by itself** on your home network, and over ZeroTier or
  Tailscale when you're away.
- **Simple app, no terminal.** A window and a tray icon. USBeam starts with your
  computer and stays out of the way.
- **Safe by default.** A shared device keeps working where it's plugged in until
  another computer starts using it, and returns there when it's released,
  disconnected or unplugged. USBeam only works on networks you trust.
- **Auto-reconnect** (optional) brings a device back after sleep or a network drop.
- **Open standard.** Uses USB/IP, so it also works with Linux's built-in USB/IP
  support and with [usbip-win2](https://github.com/vadimgrn/usbip-win2).

## Download

See [Releases](../../releases):

- **Windows** (Intel/AMD and ARM): `USBeam-Setup-<version>.exe`
- **Linux** (most distributions with systemd): `USBeam-<version>-x86_64.AppImage`
- **Arch Linux and Arch-based**: `usbeam-<version>-x86_64.pkg.tar.zst`

## Installing

**Windows:** run `USBeam-Setup-<version>.exe` and follow the steps.

**Linux (AppImage):** no terminal needed.

1. Download `USBeam-<version>-x86_64.AppImage`.
2. Right-click the file → **Properties** → **Permissions**, and tick **Allow executing file as
   program** (in some file managers: **Is executable**).
3. Double-click the file, click **Install**, and enter your password.

USBeam then starts with your computer, appears in your application menu, and updates itself
from its settings menu (the gear icon), where you can also uninstall it.

## License

USBeam is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version**. See [LICENSE](LICENSE).
