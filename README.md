# USBeam

**Free, open-source USB over IP for Windows and Linux.**

Plug a USB device into one computer and use it on another over your network,
as if it were plugged in there. Mice, keyboards, game controllers, drives and
more. No device limits, no subscriptions, no closed source.

> **Status: early development.** The core engine works and has been tested
> between Windows and Linux in both directions. The graphical app,
> installers and automatic discovery are not built yet. The current test
> programs are command-line only and meant for development.

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

## Roadmap

1. Background service + GUI app (host and client tabs in one window)
2. Automatic discovery of computers on the local network
3. Pairing / password so only your computers can take devices
4. Installers for Windows (x64 + ARM64) and Linux, firewall rules included
5. Hide devices that must never be shared (mounted drives, built-in parts, the network adapter in use)
6. Isochronous transfers (webcams, audio)
7. macOS and Android as hosts where the OS allows it

## Building

Every push to `main` is built automatically by GitHub Actions (see the
**Actions** tab for downloadable builds). To build yourself on Linux:

```
make linux                 # needs gcc
make windows-arm64         # needs llvm-mingw in PATH
make windows-x64
```

## License

USBeam is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version**. See [LICENSE](LICENSE).
