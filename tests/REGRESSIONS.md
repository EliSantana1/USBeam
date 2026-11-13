# Bugs already fixed, and what keeps them fixed

Every change is checked against this list before it's released. Each entry: what went wrong, the
cause, and the test that fails if it comes back. "Real devices" means the hand-over check on two
Linux PCs with a real Bluetooth adapter and headset (run by hand, with the owner's OK first).
Add an entry for every bug fixed from now on.

| # | What went wrong | Cause | Kept fixed by |
|---|---|---|---|
| 1 | A device let go of by the other PC was taken for an unplugged one; it then had no driver (a mouse stopped working) (0.3.25) | usbip-host resets it then; during a reset its USB address reads 0 | `restart.sh` step 5 (six normal let-gos, none an unplug) |
| 2 | A device back after an unplug was left without a driver | only usbip-host-bound devices were given back | `restart.sh` steps 3, 4 and 6 |
| 3 | A device left without a driver (or held by usbip-host) by an older version stayed that way after updating; Bluetooth pairings then silently didn't go along (0.3.26) | nothing repaired it; the failure wasn't logged | `restart.sh` step 6 (no driver while running and at start; held by usbip-host at start); both sides now log it |
| 4 | usbip-host refused a bus id for good ("bind failed: No such device") | "del" while usbip-host had the device left its entry "removed" | `restart.sh` steps 1-6 (every hand-over); give-back is unbind then rebind |
| 5 | Returned devices stayed on usbip-host's list | "del" does nothing once usbip-host has let go | `restart.sh` step 5 (off the list) |
| 6 | A device restarting itself during a hand-over was taken for an unplug, or grabbed by its own PC meanwhile | — | `restart.sh` steps 1-4, `restart_test` |
| 7 | Realtek Bluetooth adapters took over 10 s to start on the other PC (and again on return) | the next PC's Linux restarts it to load its own firmware | real devices (log: "cleared its firmware") |
| 8 | A headset took 15-20 s to connect on the other PC, or never | it was pulled away while connected and only noticed after its own timeout | real devices (connects within a few seconds) |
| 9 | Connected Bluetooth devices were often not seen as connected | busctl's result read before it had finished | `btkeys_test`; real devices |
| 10 | Bluetooth adapters on a Windows host were taken for network adapters (0.3.23) | — | `checks` (Windows builds); real devices on Windows |
| 11 | Update bar showed empty all the time | its style overrode `hidden` | `updnotice.py` |
| 12 | Every Windows update left its ~48 MB installer in the temporary folder; a cancelled Linux update left the AppImage in the cache | the installer can't remove itself | `windows-upgrade.ps1` step 4, `linux-upgrade.sh` |
| 13 | Uninstalling on a PC that uses usbip-win2 itself would remove the driver when run silently | a silent uninstall answers "Yes" | `windows-upgrade.ps1 -KeepDriver` (on the Surface) |
| 14 | Kernel tests left their time limit and test host running after they ended | background jobs not stopped | `restart.sh`, `mousefeel.sh` end cleanly (the run doesn't hang) |
| 15 | The leftover-installer cleanup (0.3.26) removed any USBeam installer, also a newer one just about to run (the 0.3.26 to 0.3.27 update test failed) | it didn't look at the version | `windows-upgrade.ps1` step 4 (a newer installer is left alone, the finished one removed); updates are tested from the latest release |

## Before every release

1. `~/usbeam-test/test-all.sh` passes: Linux VM, AppImage update test, Windows tests on the Surface.
2. Real devices (owner's OK first): Use / Release / Give To / Take back, three rounds, the
   adapter and headset following every time.
3. Updating from the previous release on a PC where it left something broken (entry 3) is covered.
4. This list was checked against the change.
