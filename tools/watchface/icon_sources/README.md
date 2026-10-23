# Watch UI icon assets

The original downloaded assets are preserved in this folder. `sources.json`
records their official Apple watchOS 27 guide / OpenAI ChatGPT App Store URLs.
These marks belong to their respective owners.

`firmware-mapping.json` records the app-to-icon mapping. For watch firmware apps
without a direct Apple equivalent, Gravitysphere uses Mindfulness, the draw
panel uses Notes, SpecAnalyzer uses Now Playing, and VideoPlayer uses Remote.
This changes artwork only; app names and actual functions remain those of the
ESP32 firmware. The ChatGPT artwork identifies the user's requested visual
theme for Xiaozhi and does not change its AI provider or protocol.

Regenerate all app icons and the overview offline:

    python tools/watchface/generate_watch_icons.py

`generate_controls.py` also restores the official Control Center glyphs after
generating the remaining hardware-specific controls. Firmware compilation reads
the committed LVGL C assets directly and does not access the network.

Only run `fetch_watch_icons.py` when intentionally updating the cached sources.
