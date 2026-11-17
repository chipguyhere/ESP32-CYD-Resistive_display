# ES3C28P (ESP32-S3) — sound

How to get sound out of the black 2.8" ESP32-S3 board's speaker connector. Unlike the
classic CYDs, this board has a real audio chip, an **ES8311** codec, so music reaches
it at the full 16 bits. The **MP3Player** example plays through it, and its output
class `AudioOutputES8311S3.h` is the code to copy. The classic boards are in
[README-sound-ESP32.md](README-sound-ESP32.md), and the circuit is in
[README-engineering.md](README-engineering.md#es3c28p-sound).

## Sound characteristics at a glance

### Signal chain

ESP32-S3 I²S port → **ES8311** codec (DAC) → **SC8002B** amplifier → the 2-pin
**SPEAKER** connector (bridge-tied: neither speaker wire is ground).

| Signal | GPIO | Notes |
|---|---|---|
| I²S MCLK | 4 | The codec's master clock, 256 × the sample rate. It must keep running while the codec is in use. |
| I²S BCLK | 5 | |
| I²S WS (LRCK) | 7 | |
| I²S data out (to the codec's DAC) | **8** | The vendor's pin table says GPIO6. That is wrong: the schematic and the vendor's own music demo use GPIO8. |
| I²S data in (from the codec's ADC) | 6 | The microphone. Not used by MP3Player. |
| I²C (codec control) | SDA 16, SCL 15 | Address **0x18**, on `Wire`, shared with the touch chip |
| Amplifier SHUTDOWN | **1** | 10 k pull-up: the amplifier is **off until GPIO1 is driven LOW** |

**Mono.** The amplifier and speaker are mono; the codec plays its left/right mix.
`AudioOutputES8311S3` sends the stereo average to both slots.

**Gain.** The amplifier's gain is about 2× (6 dB). With the codec's volume at 0 dB,
a full-scale track is loud on a small speaker and clean *(bench, 2026-11-09)*.

**Audio power.** The codec has its own 3.3 V regulator, always on. There is no switch
for it, only the amplifier's.

### Getting samples out

The codec is an I²C-configured DAC in slave mode. The ESP32-S3 drives MCLK, BCLK and
WS, and the codec's dividers follow MCLK, so any rate from 8 to 48 kHz works without
a register change. The sequence `AudioOutputES8311S3` uses:

1. `Wire.begin(16, 15, 400000)`. This is a no-op if the touch driver already started it.
2. Start an I²S standard (Philips) channel, 16-bit stereo, MCLK = 256 fs, with
   **`auto_clear` on**, so an empty DMA queue sends silence instead of repeating the
   last buffer.
3. Configure the codec over I²C (the ES8311's reset, clock, serial-port and DAC
   power-up registers; the full list is in the file).
4. Set the volume with the codec's DAC volume register (0x32: 0xBF = 0 dB, 0.5 dB
   per step), and mute with 0x31.
5. Drive **GPIO1 LOW** to turn on the amplifier, only once the codec is running.

ESP8266Audio's own `AudioOutputI2S` is not used. It stops the I²S port between
tracks, which stops MCLK and pops. `AudioOutputES8311S3` creates the channel once and
leaves it running.

### Limits that keep it clean

- **Keep MCLK running** from before the amplifier comes on until after it goes off.
  The codec loses its clock otherwise, and the amplifier passes the click.
- **Amplifier off while idle.** MP3Player turns it off when stopped or paused, and on
  only while the decoder runs.
- **Volume in the codec, not in software.** Turning the samples down in software
  throws bits away. The codec's register keeps all 16 bits at any volume.
- **Write without blocking for long.** The output writes in ~6 ms chunks and returns
  `false` once per chunk, so `AudioGeneratorMP3::loop()` comes back to its task. See
  MP3Player's `player.cpp` for the task and its pacing.

### Power interaction

The amplifier runs from the 5 V rail, so on battery it runs from the cell's 3.4–4.2 V
and has less headroom. **TODO (bench):** loudness and clipping on battery at full
volume.

### Recommendations

- Start from **MP3Player**: copy `AudioOutputES8311S3.h` and `platform_ESP32S3.cpp`.
- For beeps and short sounds, use the same output class: start it once, then write
  generated samples to it.
- If a sketch also uses the microphone, add GPIO6 as the I²S channel's data-in and
  enable the codec's ADC path. The register sequence here sets up only the DAC.
