Lody Panel
==========

Lody Panel turns a Phat-Stats ESP32 + ILI9341 (320x240) board into a desk
status display for Lody. The panel shows one glanceable state at a time:
working / needs you / done / quiet / check / link lost, plus the active
session title, provider logo, model, clock and counts.

Data path
---------
Lody CLI -> bridge (host) -> USB serial (115200, NDJSON) -> ESP32.

The bridge reads only Lody session metadata (title, state, timestamps,
project, agent type, model name). It never sends chat content, commands,
diffs, tool output or credentials to the panel.

Layout
------
- firmware/   Arduino sketch for ESP32 + ILI9341
- bridge/     Python standard-library bridge and preview HTTP server
- preview/    Browser preview of the same 320x240 layout
- tools/      Reproducible status-glyph generator
- preview/icons/  Provider logos

Build firmware
--------------
1. Install Arduino CLI and esp32 core 2.0.5.
2. Install libraries: Adafruit GFX, Adafruit ILI9341, Adafruit BusIO,
   ArduinoJson (v7), U8g2, U8g2_for_Adafruit_GFX.
3. Compile:

   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app lody-panel/firmware/LodyPanel

4. Connect the board (/dev/ttyUSB0) and upload:

   cd lody-panel/firmware && ./build.sh upload

Run bridge / preview
--------------------
   python3 lody-panel/bridge/lody_bridge.py --port /dev/ttyUSB0 --serve
   # preview: http://127.0.0.1:8789/

   # no serial, print frames:
   python3 lody-panel/bridge/lody_bridge.py --once --dry-run

The bridge uses the current Lody workspace unless --workspace is given.

Privacy
-------
No cloud account, no telemetry, no chat transcript. The panel receives a
small status snapshot only. Keep flash backups and local docs out of git;
see .gitignore.

Licenses
--------
Lody Panel is part of Phat-Stats and is distributed under GPL-2.0
(see ../LICENSE.txt). Third-party notices: NOTICE.txt.
