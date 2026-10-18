# Punto Switcher2.8.14

## Changes

- Coherent X11 daemon, authenticated session helper, GNOME RU/EN bridge and tray.
  Startup requires typed desktop readiness instead of reporting partial success.
- Manual Pause conversion, Shift+Pause selection conversion and inverse/undo
  keep focus, text, layout, clipboard and modifier guards aligned.
- Explicit systemd state handling: transitions and failed services no longer
  masquerade as stopped writers. Helper retirement waits for backend shutdown.
- Session-helper protection permits read-only access to the user session bus.
- Sound settings open their own dialog instead of the general settings window.
- Bounded default build parallelism, explicit OpenSSL dependencies, package
  lifecycle/recovery checks and extended real-layout GTK/Chromium coverage.

## Installation and upgrades

The Debian package is built by `./build-deb.sh`; this command does not install
software or restart the running service. Install the release DEB through the
normal package manager. Preserve user configuration and learned exclusions.

When upgrading an already loaded GNOME extension, save documents and log out,
then log back in using Ubuntu/GNOME on Xorg. A machine reboot or forced Shell/
IBus restart is not required. Use `punto start` and `punto status` after login.

## Validation and boundaries

Qualified private suites cover GTK209, real-layout GTK/VTE14, Chromium114 plus
two real-layout cases, repeated conversion/recovery, sanitizers, package lifecycle
and exact-old-version recovery. Installed-host checks independently exercise
supported GTK/Chrome manual, selection, next-glyph, empty recovery, Space-auto,
clipboard and modifier behavior through the installed daemon/uinput pipeline.
These finite checks are not a guarantee that every future failure is impossible.

- The normal supported desktop is GNOME46 on X11; no full Wayland backend.
- No browser/editor extension is required. The GNOME layout bridge is required.
- Automatic Tab correction requires a fully portable clipboard snapshot. Private
  Chromium targets such as `chromium/x-source-url` are not silently discarded:
  correction is refused, clipboard retained, and the original Tab is delivered.
- Compose/dead-key/AltGr input and arbitrary editor callback atomicity are not
  promised. Unknown focus, selection, clipboard or delivery outcomes fail closed.
- The current host run is software-pipeline validation, not physical-device
  qualification or a new long-duration idle soak. Historical soak evidence is
  identified separately in RESTORATION.md.
