<div align="center"><img src="assets/icon.png" alt="BlastIRC" width="96"></div>

# BLAST IRC

A native IRCv3 client for x86_64 Linux and Wayland; X11 and Xwayland are not supported.

## Getting started

From a checkout with Nix:

```sh
nix run .
```

To build separately, use `nix build .#default` and run `./result/bin/blastirc`; for development setup, see [Development and design](UI-DESIGN.md#verification-contract).

On the welcome screen, enter your server, port, nickname and identity, keep TLS enabled, add any auto-join channels, and select **Save and connect**.

For soju, choose **Add soju account** in **Settings → Networks**, enter the bouncer endpoint and base-account identity without a `/network` suffix, then set **Password (SASL PLAIN)** credentials and select **Save and connect**.

## Features

- Connect directly or through ZNC and soju with TLS, SASL authentication and native soju network management.
- Choose messenger bubbles, adaptive traditional IRC rows or Traditional · two-line chat styles.
- Use keyboard navigation, command and nickname completion, emoji completion and Emacs-style editing.
- Exchange native IRCv3 typing indicators, replies, reactions and read markers, and load history when supported by the server or bouncer.
- Search local history and receive desktop mention notifications.
- Configure independent UI and message fonts and weights, Noctalia colors, scale, transparency, motion and sound.
- Review uploads before sharing links and load link, image and media previews on click by default.

[MIT license](LICENSE) · [Development and design](UI-DESIGN.md)
