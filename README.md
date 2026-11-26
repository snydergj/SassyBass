# SassyBass 🐟

A Big Mouth Billy Bass turned into a hands-free AI chatbot. Say **"Hey Billy"**, chat, and say **"goodbye"** when you're done.

- **Board:** Waveshare ESP32-S3-AUDIO-Board, with a DRV8833 driving the mouth and body motors
- **AI:** Google Gemini Live API (free tier)
- **Installer:** https://snydergj.github.io/SassyBass/ (flash from the browser over USB)
- **Updates:** each fish checks GitHub Pages for new firmware every time it boots, and once a day after that

## Repo layout

| Path | What it is |
|---|---|
| `firmware/SassyBass/` | Arduino sketch. Personality, voice, and pins are in `config.h` |
| `docs/` | GitHub Pages site: installer page, `manifest.json`, `version.json`, published binaries |
| `.github/workflows/build.yml` | Cloud build. Compiles on every push; publishes on `v*` tags |

## One-time GitHub setup

Go to **Settings → Pages → Build and deployment → Deploy from a branch → `main` / `/docs`**.

## Releasing an update

1. Edit the firmware and push to `main`. GitHub Actions compiles it to check for errors.
2. Create a version tag: **Releases → Draft a new release → tag `v1.0.1` → Publish**, or `git tag v1.0.1 && git push origin v1.0.1`.
3. Actions builds the binaries, copies them into `docs/firmware/`, and bumps `docs/version.json` and `docs/manifest.json`.
4. Every fish updates the next time it starts. The web installer also serves the new version.

Version numbers must keep increasing (`1.0.0` → `1.0.1` → `1.1.0`). Local Arduino IDE builds report `dev` and never auto-update, so your test builds aren't overwritten.

## Setting up a new fish (owner)

1. Flash it from the installer page and **erase** it, since it's new.
2. Billy's light turns purple. Join `BillyBass-Setup` (password `billybass`).
3. On the setup page, enter the Wi-Fi details **and the Gemini API key**. The key field shows automatically when no key is saved. To change the key later, use `http://192.168.4.1/?owner=1` while the fish is in setup mode (hold the red button or K1 at power-up).
   Over USB serial you can also send `key=AIza...`, `wifi=SSID,password`, or `restart`.
4. Before you gift it, re-run setup so it's waiting for the new owner's Wi-Fi. The key stays saved.

The key is stored only in the fish's settings storage (NVS). **It is never in the firmware or in this repo.** Updates keep it; erasing the board wipes it.

**Getting a key:** go to https://aistudio.google.com/apikey and create the key in a project **with no billing attached**, so it can only use the free tier. Using one key per fish keeps each fish's free quota separate.

## Local builds (Arduino IDE)

- esp32 core 3.3.12 or newer. Board: **ESP32S3 Dev Module**. Flash Size: 16MB.
- **Partition Scheme: ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)**. PSRAM: OPI PSRAM. USB CDC On Boot: Enabled.
- No extra libraries are needed. The `es8311` codec driver (Espressif, Apache-2.0) is included in the sketch folder.
- Optional: copy `secrets_example.h` to `secrets.h` (git-ignored) to pre-load your key on first boot.

## Wiring

| From | To |
|---|---|
| GPIO4 / GPIO5 (LCD header) | DRV8833 AIN1 / AIN2 → mouth motor |
| GPIO7 / GPIO8 (LCD header) | DRV8833 BIN1 / BIN2 → body motor (forward = head, reverse = tail) |
| GPIO9 | Billy's red button (other leg to GND) |
| 5 V, 2 A+ | Board USB-C **and** DRV8833 VM, with a 470 µF cap across VM-GND |
| Billy's speaker | Board speaker connector |

## Using it

- Say **"Hey Billy"**, chat, and say **"goodbye"** to end. The chat also ends after 45 seconds of silence.
- The red button (or BOOT or K2) starts or ends a chat.
- Ask for louder or quieter, or press K1 (down) / K3 (up).
- Ask him to dance, flap his tail, or change his light color. These are Gemini function calls handled in `runTool()`.
- LED colors: purple = setup, blue = listening, green = talking, white = updating, red = error.
