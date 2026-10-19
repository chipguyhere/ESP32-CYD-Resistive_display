/*
 * AudioOutputCydDac.h - ESP8266Audio output for the resistive-touch CYD speaker.
 *
 * Why not the library's own AudioOutputInternalDAC?  In ESP8266Audio 2.4.1 it
 * enables DAC_CHANNEL_MASK_CH0 (GPIO25) for mono and BOTH channels for stereo,
 * and its constructor drives GPIO25 low.  On this board GPIO25 is the XPT2046
 * touch clock, so touch dies the moment audio starts.  See README-sound.md in
 * chipguy_ESP32-CYD-Resistive_display.
 *
 * This class follows that README's rules instead:
 *   - dac_continuous on DAC channel 1 (GPIO26) ONLY, DMA-fed.
 *   - Clocked from the default PLL, never the APLL.  With
 *     DAC_DIGI_CLK_SRC_APLL the output carries random full-scale spikes
 *     whenever the code is changing — audible as volume-independent static
 *     that vanishes in digital silence (found on this board, 2026-09-18; the
 *     same fingerprint as arduino-esp32 issue #8467 on the legacy driver).
 *     The default clock only reaches down to 19.6 kHz, so lower rates are
 *     upsampled by the smallest integer factor that gets above it (8 kHz x3,
 *     11.025/16 kHz x2), with linear interpolation — plenty for an 8-bit DAC
 *     behind a 36 kHz RC filter.
 *   - Stereo is downmixed to mono; the amplifier and speaker are mono.
 *   - Full-scale output by default.  README-sound.md's inferred +/-80 ceiling
 *     was tested against real music and found unnecessary: no audible
 *     clipping at +/-127.  setMaxSwing() is still there if a board wants it.
 *   - The DAC is ramped from 0 to mid-scale before the first sample and back
 *     down before it is ever disabled, so the 0.39 uF coupling cap doesn't
 *     thump the speaker.  Between tracks it stays enabled, parked at 128.
 *   - The amplifier has a shutdown pin on this board: GPIO4, active LOW,
 *     with a 10 k pull-up so it is OFF at reset.  This class turns it on
 *     only after the ramp above has settled at mid-scale and off before
 *     ramping back down, so the DAC's enable/disable steps are never
 *     amplified.  setAmpEnabled() lets the player mute it while paused or
 *     stopped.  (The ESP32-2432S028 and ESP32-024 have no such pin; their
 *     amps are always on.)
 *   - Optional TPDF dither with first-order noise shaping.  Undithered, an
 *     8-bit DAC turns quiet passages into random clicks: each 1-LSB code step
 *     of a slowly-moving signal is a discrete edge, always the same size
 *     regardless of volume, and absent only in digital silence.  Dither
 *     decorrelates the steps into a faint steady hiss, and the error feedback
 *     pushes that hiss up toward fs/2 where the speaker and the ear don't
 *     follow.
 *
 * ConsumeSample() blocks when the DMA queue is full, which is what paces the
 * decoder.  Run the decoder in its own FreeRTOS task, not next to LVGL.
 * It also returns false once per chunk written: AudioGeneratorMP3::loop()
 * keeps decoding until ConsumeSample() refuses a sample, so an output that
 * always accepts would keep loop() busy for the whole file and starve the
 * caller.  The decoder holds the refused sample and re-offers it next call.
 *
 * Every dac_continuous_write() here is exactly one chunk of samples, padded
 * with the hold level when there isn't enough real data.  The IDF driver turns
 * each write into an I2S DMA descriptor, and a descriptor whose length isn't
 * word-aligned stalls the DMA: the EOF interrupt never returns descriptors to
 * the pool and the next write blocks forever.  (Its s_dac_wait_to_load_dma_data
 * also halves sub-buffer-size writes on alternate calls, so odd sizes get
 * worse, not better.)  Fixed whole chunks sidestep all of that.
 */

#pragma once

#include <Arduino.h>
#include <AudioOutput.h>
#include <driver/dac_continuous.h>
#include <chipguy_CYD_resistive_display.h>

class AudioOutputCydDac : public AudioOutput {
public:
    // Peak swing around mid-scale at 100% volume, in DAC counts (max 127).
    static constexpr int kFullScaleSwing = 127;

    // Amplifier shutdown: GPIO4, LOW = amplifier on.  See README-sound.md.
    static constexpr int kAmpEnablePin = 4;

    // The ESP32-32E boards have the shutdown pin; on the ESP32-2432S028 and ESP32-024 GPIO4
    // is the red LED instead and the amp is always on.  Told apart by the red
    // LED's pin, which display.begin() has already latched.
    static bool hasAmpPin() { return cydr_redLedPin() != 4; }

    // Below this rate the default 160 MHz PLL can't divide down far enough
    // (mclk_div would exceed 255); input at a lower rate is upsampled.
    static constexpr int kMinDacHz = 19600;

    // DMA queue: kDmaDescs descriptors of kDmaBufBytes (16-bit slots, so
    // half that many samples each).  12 x 1024 samples is ~280 ms at
    // 44.1 kHz in the best case, ~190 ms after the driver's split quirk —
    // enough to ride out an SD read stall.  24 KB of DMA-capable RAM.
    static constexpr int kDmaDescs = 12;
    static constexpr int kDmaBufBytes = 2048;

    // chunkSamples: samples per DMA write.  Keep it equal to kDmaBufBytes —
    // the IDF driver halves smaller writes on alternate calls, which leaves
    // descriptors half-empty and shrinks the queue.
    explicit AudioOutputCydDac(size_t chunkSamples = kDmaBufBytes, int maxSwing = kFullScaleSwing)
        : _chunkSize(chunkSamples), _maxSwing(maxSwing) {
        _chunk = (uint8_t *)malloc(_chunkSize);
        hertz = 44100;
        channels = 2;
        // Amp off until the DAC is running and parked at mid-scale.
        if (hasAmpPin()) {
            pinMode(kAmpEnablePin, OUTPUT);
            digitalWrite(kAmpEnablePin, HIGH);
        }
    }

    ~AudioOutputCydDac() override {
        teardown();
        free(_chunk);
    }

    // 0..100.  Applied per sample, so it can be changed during playback from
    // another task (a byte store is atomic on Xtensa).
    void setVolume(uint8_t pct) {
        if (pct > 100) pct = 100;
        _volume = pct;
    }
    uint8_t volume() const { return _volume; }

    // Change the amplitude ceiling (1..127).  Takes effect on the next sample.
    void setMaxSwing(int counts) {
        _maxSwing = counts < 1 ? 1 : counts > kFullScaleSwing ? kFullScaleSwing : counts;
    }
    int maxSwing() const { return _maxSwing; }

    // Amplifier on/off (GPIO4, active LOW).  Only meaningful while the DAC is
    // running; ensureRunning()/teardown() sequence it around the ramps.
    // No-op on the ESP32-2432S028 and ESP32-024, which have no such pin.
    void setAmpEnabled(bool on) {
        if (hasAmpPin()) digitalWrite(kAmpEnablePin, on ? LOW : HIGH);
        _ampOn = on;
    }
    bool ampEnabled() const { return _ampOn; }

    // Rate the decoder last set (AudioOutput keeps it protected).
    int sampleRate() const { return hertz; }

    // TPDF dither + first-order noise shaping on the 8-bit quantizer.  Off
    // is plain round-to-nearest, for A/B listening.
    void setDither(bool on) { _dither = on; if (!on) _err = 0; }
    bool dither() const { return _dither; }

    // Upsampling factor in use (1 = none) and the rate the DAC is running at.
    int upsample() const { return _upsample; }
    int dacRate() const { return hertz * _upsample; }

    // Times the DMA chain ran empty mid-stream (the driver's on_stop event).
    // Each one is an audible click: the I2S TX clocks out zeros while it has
    // nothing to send, so the DAC steps from mid-scale to 0 V regardless of
    // volume.  Deliberate stops (flush, ramp) are excluded.
    uint32_t underruns() const { return _underruns; }

    // ---- AudioOutput interface --------------------------------------------

    bool begin() override {
        // The decoder calls begin() before it knows the real rate; SetRate()
        // reconfigures once the first frame is decoded.
        return ensureRunning();
    }

    bool SetRate(int hz) override {
        if (hz == hertz) return true;
        hertz = hz;
        _upsample = 1;
        while (hertz * _upsample < kMinDacHz) _upsample++;
        if (_dac) {
            // dac_continuous can't change frequency in place: drain, ramp
            // down, rebuild the channel at the new rate, ramp back up.
            teardown();
            return ensureRunning();
        }
        return true;
    }

    bool SetChannels(int ch) override {
        if (ch < 1 || ch > 2) return false;
        channels = ch;
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!_dac) return false;

        // A chunk went out on the previous call: refuse this sample so the
        // decoder's loop() returns to its caller.  It will be offered again
        // on the next loop().
        if (_yield) {
            _yield = false;
            return false;
        }

        int32_t mono = (channels == 2)
            ? ((int32_t)sample[0] + sample[1]) / 2
            : sample[0];

        // Upsampling: emit _upsample points on the line from the previous
        // input sample to this one (the last point is this sample itself).
        if (_upsample == 1) {
            emit(mono);
        } else {
            for (int k = 1; k <= _upsample; k++)
                emit(_prevMono + ((mono - _prevMono) * k) / _upsample);
        }
        _prevMono = mono;
        return true;
    }

    // Push whatever is pending (padded to a whole chunk with mid-scale), then
    // one more chunk of mid-scale so the line is parked at 128.
    void flush() override {
        if (!_dac) return;
        memset(_chunk + _fill, 128, _chunkSize - _fill);
        writeChunk();
        memset(_chunk, 128, _chunkSize);
        writeChunk(true);
        _lastLevel = 128;
    }

    // The decoder calls stop() at the end of every track.  Park at mid-scale
    // but leave the DAC enabled: disabling it drops GPIO26 to 0 V and pops.
    bool stop() override {
        flush();
        return true;
    }

private:
    dac_continuous_handle_t _dac = nullptr;
    uint8_t *_chunk;
    size_t _chunkSize;
    size_t _fill = 0;
    volatile uint8_t _volume = 70;
    volatile int _maxSwing;
    volatile bool _dither = true;
    int _upsample = 1;
    int32_t _prevMono = 0;
    bool _yield = false;
    int32_t _err = 0;        // noise-shaping error feedback, in 1/32768 count
    uint32_t _rng = 0x9E3779B9;
    int _lastLevel = 0;      // last value handed to the DAC, for ramping
    bool _ampOn = false;


    // Volume, quantize to 8 bits (dithered or rounded), store; write the
    // chunk when it fills.
    void emit(int32_t mono) {
        // Scale to +/-_maxSwing at full volume.  Work in units where one
        // DAC count is 32768, so the fractional part is what the quantizer
        // below throws away (max |mono*127*100| = 4.2e8, fits int32).
        int32_t x = (mono * _maxSwing * (int32_t)_volume) / 100;
        int32_t q;
        if (_dither) {
            // Error feedback: add back what the previous sample lost, then
            // TPDF dither of +/-1 LSB (sum of two uniforms), round, and
            // remember the new error.  Clamping the error keeps the loop
            // sane if the signal sits at the ceiling.
            int32_t v = x + _err;
            int32_t d = (int32_t)(xorshift() & 0x7FFF) + (int32_t)(xorshift() & 0x7FFF) - 32768;
            q = (v + d + 16384) >> 15;
            if (q > _maxSwing) q = _maxSwing; else if (q < -_maxSwing) q = -_maxSwing;
            _err = v - (q << 15);
            if (_err > 32768) _err = 32768; else if (_err < -32768) _err = -32768;
        } else {
            q = (x + (x >= 0 ? 16384 : -16384)) / 32768;      // round to nearest
        }
        _chunk[_fill++] = (uint8_t)(128 + q);
        _lastLevel = 128 + q;
        if (_fill == _chunkSize) {
            writeChunk();       // blocks until the DMA has room
            _yield = true;
        }
    }

    uint32_t xorshift() {
        uint32_t x = _rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return _rng = x;
    }
    volatile uint32_t _underruns = 0;
    volatile bool _expectStop = false;   // the chain is about to run dry on purpose

    static bool IRAM_ATTR onStop(dac_continuous_handle_t, const dac_event_data_t *, void *arg) {
        auto *self = (AudioOutputCydDac *)arg;
        if (!self->_expectStop) self->_underruns++;
        return false;
    }

    // Generous: a chunk at the lowest MP3 rate (8 kHz) is 64 ms, and the DMA
    // queue holds ~4 chunks.  If the driver ever takes this long it has
    // stalled, and dropping the chunk beats wedging the audio task.
    static constexpr int kWriteTimeoutMs = 1000;

    // Write exactly one chunk (_chunk[0.._chunkSize)) and reset the fill.
    // finalChunk: nothing follows, so the chain draining afterwards is not
    // an underrun.
    void writeChunk(bool finalChunk = false) {
        _expectStop = finalChunk;
        esp_err_t err = dac_continuous_write(_dac, _chunk, _chunkSize, nullptr, kWriteTimeoutMs);
        if (err != ESP_OK)
            Serial.printf("dac_continuous_write: %s\n", esp_err_to_name(err));
        _fill = 0;
    }

    bool ensureRunning() {
        if (_dac) return true;

        if (_upsample > 1)
            Serial.printf("AudioOutputCydDac: %d Hz upsampled x%d to %d Hz\n",
                          hertz, _upsample, hertz * _upsample);

        dac_continuous_config_t cfg = {
            .chan_mask = DAC_CHANNEL_MASK_CH1,    // GPIO26 only — GPIO25 is touch CLK
            .desc_num  = kDmaDescs,
            .buf_size  = kDmaBufBytes,
            .freq_hz   = (uint32_t)(hertz * _upsample),
            .offset    = 0,
            .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,   // never the APLL — see header
            .chan_mode = DAC_CHANNEL_MODE_SIMUL,
        };
        esp_err_t err = dac_continuous_new_channels(&cfg, &_dac);
        if (err != ESP_OK) {
            Serial.printf("dac_continuous_new_channels: %s\n", esp_err_to_name(err));
            _dac = nullptr;
            return false;
        }
        dac_event_callbacks_t cbs = { .on_convert_done = nullptr, .on_stop = onStop };
        err = dac_continuous_register_event_callback(_dac, &cbs, this);
        if (err != ESP_OK)
            Serial.printf("dac_continuous_register_event_callback: %s (underrun count disabled)\n",
                          esp_err_to_name(err));
        err = dac_continuous_enable(_dac);
        if (err != ESP_OK) {
            Serial.printf("dac_continuous_enable: %s\n", esp_err_to_name(err));
            dac_continuous_del_channels(_dac);
            _dac = nullptr;
            return false;
        }
        _fill = 0;
        _yield = false;
        _prevMono = 0;
        _err = 0;
        ramp(0, 128);       // pin was at 0 V; ease up to the idle level
        setAmpEnabled(true);   // only now, with the input settled at mid-scale
        return true;
    }

    void teardown() {
        if (!_dac) return;
        flush();
        setAmpEnabled(false);  // before the ramp down, so the step to 0 V is never heard
        ramp(_lastLevel, 0);
        dac_continuous_disable(_dac);
        dac_continuous_del_channels(_dac);
        _dac = nullptr;
    }

    // Linear ramp over ~200 ms (many times the ~10 ms coupling time constant
    // on this board; the amp is off during it anyway),
    // rounded up to whole chunks; the last sample lands exactly on `to`.
    void ramp(int from, int to) {
        uint32_t total = ((uint32_t)(hertz * _upsample) / 5 + _chunkSize - 1) / _chunkSize * _chunkSize;
        uint32_t i = 0;
        while (i < total) {
            for (size_t k = 0; k < _chunkSize; k++, i++)
                _chunk[k] = (uint8_t)(from + ((to - from) * (int32_t)i) / (int32_t)(total - 1));
            writeChunk(i >= total);
        }
        _lastLevel = to;
    }
};
