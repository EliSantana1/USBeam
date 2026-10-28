# USBeam

**Free, open-source USB over IP for Windows and Linux.**

Plug a USB device into one computer and use it on another over your network,
as if it were plugged in there. Mice, keyboards, game controllers, drives and
more. No device limits, no subscriptions, no closed source.

> **Status: early development (0.2).** Sharing and using devices works between
> Windows and Linux in both directions, with automatic
> discovery, a graphical app, a background service and installers being tested.

## Goals

- **One app, both roles.** Every installation can share its devices (host)
  and use devices from other computers (client), switchable any time.
- **No terminal, ever.** A simple GUI for everything; admin rights only through
  the normal system prompts.
- **Works with nobody at the host.** The host runs as a background service and
  offers its devices; you pick and release devices from the client.
- **Safe by default.** A shared device keeps working on the host until a client
  actually connects. Released, disconnected or unplugged devices always return
  to the host. Auto-reconnect is optional and shows a cancellable countdown.
- **Open standard.** Uses the USB/IP protocol, so it also works with the
  Linux kernel's built-in USB/IP support and with
  [usbip-win2](https://github.com/vadimgrn/usbip-win2).

## How it works

| | Sharing (host) | Using (client) |
|---|---|---|
| **Windows** | Switches the device to Microsoft's built-in, Microsoft-signed **WinUSB** driver only while a client uses it, then restores its normal driver. No custom kernel driver or certificate. | Uses the signed **usbip-win2** driver (planned to be built into the app). |
| **Linux** | Uses the kernel's built-in **usbip-host** driver. | Uses the kernel's built-in **vhci-hcd** driver, with auto-reconnect and automatic speed tuning for drives. |

## Tested so far

- Windows → Linux: mouse, USB flash drive (byte-for-byte verified), several devices at once.
- Linux → Windows: game controller receiver.
- Unplug / replug, network drops, auto-reconnect with countdown, returning devices to the host.

Not supported yet: isochronous devices (webcams, USB audio).

## How it's put together

- **The USBeam service** does the USB work. It starts with the computer
  (a Windows service / a systemd service) and needs nobody at the keyboard.
- **The USBeam app** is a tray icon plus a native window (WebView2 on Windows,
  Qt WebEngine on Linux, matching KDE Plasma). Closing the window keeps USBeam
  running in the tray.
- **Networks:** each PC appears once, with every address it is reachable at
  (home network, ZeroTier, Tailscale); "Connect over" picks which one to use.
- **Auto-reconnect** survives sleep and hibernate, but is cleared when a PC is
  shut down or restarted.

## Roadmap

1. Pairing / password so only your computers can take devices
2. Bundle the usbip-win2 driver in the Windows installer
3. Packages for more Linux distributions (Debian/Ubuntu, Fedora)
4. Isochronous transfers (webcams, audio)
5. macOS and Android as hosts where the OS allows it

## Building

Every push to `main` is built automatically by GitHub Actions (see the
**Actions** tab): a Windows installer (x64 + ARM64) and an Arch Linux package.
See the `Makefile` and `packaging/` for building by hand.

## License

USBeam is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version**. See [LICENSE](LICENSE).
