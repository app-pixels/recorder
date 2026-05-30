# recorder

**Recorder** · v1.0.0

Voice recorder. Saves WAV files to the SD card.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#audio` `#offline`

Press to record, press again to stop. Files are written to `/recordings/` on the SD card as 16 kHz 16-bit mono WAV.

## Controls
- **BOOT** — start / stop recording
- **PWR** — play back the most recent take

## Setup
No `setup.txt` needed.

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/recorder_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/recorder_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/recorder).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/recorder
