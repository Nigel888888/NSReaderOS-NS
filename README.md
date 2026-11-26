# NSReaderOS

A calm, battery-friendly reading firmware for the Read Pico (ESP32-S3) e-ink reader.
NSReaderOS is a personalized build focused on a clean, distraction-free reading
experience and long standby time.

## Highlights

- **Distraction-free library** — cover-shelf view with per-book progress bars,
  recent / now-reading shelves, and a refined card-style UI.
- **Reading tools** — vocabulary notebook with Anki export, favourite-quotes
  collection, bookmarks, dictionaries and adjustable typography.
- **RSS reader** — subscribe to feeds, read offline with in-page navigation,
  long-press to look up words or save quotes, and one-tap clearing of read items.
- **TickTick agenda** — read-only ICS calendar subscription (Today / Week / All),
  with your subscription URL stored locally only.
- **Weather** — 10-day forecast with condition text, refreshed on demand.
- **Screensaver** — picture, calendar clock, book collage and reading-receipt styles.
- **Reading statistics** — daily / weekly goals, streaks, and a rotating favourite quote.
- **Sync & transfer** — WebDAV (Nutstore), OPDS, Wi-Fi and USB file transfer.
- **Four refresh modes** — Ultra / HD / Fast / Fastest, applied across the whole UI
  to balance ghosting against flicker.
- **Source Han Sans UI font** — a unified, legible typeface across the system.

## Devices

Built for the Read Pico (ESP32-S3, 8 MB PSRAM, EPD). Flash `firmware.bin` at
`0x10000` for an app-only upgrade (keeps your books and settings), or use the
complete flash bundle for a full install.

## Firmware files

Each release provides:

| File | Use |
|---|---|
| `firmware.bin` | App-only upgrade at `0x10000` (keeps data / TF card) |
| `firmware.factory.bin` | Factory image |
| `bootloader.bin`, `partitions.bin`, `boot_app0.bin` | Boot components for a full flash |
| `SHA256SUMS` | Checksums for all images |
| `NSReaderOS-*-complete-flash.zip` | One-click full flash bundle + scripts |

## Acknowledgement

NSReaderOS is a personalized fork of [RickyOS](https://github.com/chinoryunqin/RickyOS)
(MIT License). It retains the original copyright notices and license text as
required by the MIT License. See [LICENSE](LICENSE).

## License

MIT. See [LICENSE](LICENSE).
