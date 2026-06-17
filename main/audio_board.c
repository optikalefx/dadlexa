#include "audio_board.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_PORT I2C_NUM_0
#define I2S_PORT I2S_NUM_0
#define MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define TCA9555_ADDR 0x20
#define TCA9555_INPUT_PORT_1 0x01
#define TCA9555_OUTPUT_PORT_1 0x03
#define TCA9555_CONFIG_PORT_1 0x07
#define SPEAKER_PA_BIT 0
#define KEY1_BIT 1
#define KEY2_BIT 2
#define WAV_HEADER_BYTES 44
#define RECORD_VAD_FRAME_MS 20
#define RECORD_VAD_FRAME_SAMPLES (RECORD_SAMPLE_RATE * RECORD_VAD_FRAME_MS / 1000)

static const char *TAG = "audio_board";

static i2s_chan_handle_t tx_handle;
static i2s_chan_handle_t rx_handle;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t tca_handle;
static esp_codec_dev_handle_t speaker_codec;
static esp_codec_dev_handle_t mic_codec;
static int current_sample_rate;
static bool i2s_channels_enabled;
static bool speaker_open;
static bool mic_open;
static SemaphoreHandle_t audio_mutex;

/* Data preparation helper: writes a 16-bit integer in WAV little-endian order;
 * it does not touch the audio hardware. */
static void write_le16(uint8_t *target, uint16_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
}

/* Data preparation helper: writes a 32-bit integer in WAV little-endian order;
 * it is used only when building the recorded-audio buffer. */
static void write_le32(uint8_t *target, uint32_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
    target[2] = (value >> 16) & 0xff;
    target[3] = (value >> 24) & 0xff;
}

/* Data preparation helper: constructs the in-memory mono 16 kHz WAV header for
 * audio captured from the microphone. */
static void write_wav_header(uint8_t *wav, uint32_t data_bytes)
{
    memcpy(wav, "RIFF", 4);
    write_le32(wav + 4, 36 + data_bytes);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    write_le32(wav + 16, 16);
    write_le16(wav + 20, 1);
    write_le16(wav + 22, 1);
    write_le32(wav + 24, RECORD_SAMPLE_RATE);
    write_le32(wav + 28, RECORD_SAMPLE_RATE * 2);
    write_le16(wav + 32, 2);
    write_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    write_le32(wav + 40, data_bytes);
}

/* Hardware boundary: reads one register from the TCA9555 I/O expander over I2C,
 * which is how this board exposes amp and button control lines. */
static esp_err_t tca_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(tca_handle, &reg, 1, value, 1, pdMS_TO_TICKS(1000));
}

/* Hardware boundary: writes one register on the TCA9555 I/O expander over I2C. */
static esp_err_t tca_write(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(tca_handle, payload, sizeof(payload), pdMS_TO_TICKS(1000));
}

/* Hardware boundary: configures and drives the expander pin that enables the
 * speaker power amplifier. */
static esp_err_t enable_speaker_amp(void)
{
    uint8_t config = 0xff;
    ESP_RETURN_ON_ERROR(tca_read(TCA9555_CONFIG_PORT_1, &config), TAG, "tca config read");
    config &= ~(1U << SPEAKER_PA_BIT);
    ESP_RETURN_ON_ERROR(tca_write(TCA9555_CONFIG_PORT_1, config), TAG, "tca config write");

    uint8_t output = 0;
    ESP_RETURN_ON_ERROR(tca_read(TCA9555_OUTPUT_PORT_1, &output), TAG, "tca output read");
    output |= (1U << SPEAKER_PA_BIT);
    return tca_write(TCA9555_OUTPUT_PORT_1, output);
}

/* Hardware boundary: creates the ESP-IDF I2C master bus and attaches the board's
 * TCA9555 expander device. */
static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c bus");

    i2c_device_config_t tca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(i2c_bus, &tca_cfg, &tca_handle);
}

/* Hardware boundary: creates and enables the shared I2S TX/RX channels used by
 * both the speaker DAC and microphone ADC codecs. */
static esp_err_t init_i2s(int sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCLK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DOUT_IO,
            .din = I2S_DIN_IO,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "rx std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_handle), TAG, "enable rx");
    i2s_channels_enabled = true;
    current_sample_rate = sample_rate;
    return ESP_OK;
}

/* Hardware setup helper: wraps the current I2S channels in the codec data
 * interface expected by the ESP audio codec layer. */
static const audio_codec_data_if_t *new_i2s_data_if(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_PORT,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
        .clk_src = I2S_CLK_SRC_DEFAULT,
    };
    return audio_codec_new_i2s_data(&i2s_cfg);
}

/* Hardware setup helper: wraps an I2C codec address in the control interface
 * used to configure the speaker or microphone codec chips. */
static const audio_codec_ctrl_if_t *new_i2c_ctrl_if(uint8_t addr)
{
    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_PORT, .addr = addr, .bus_handle = i2c_bus};
    return audio_codec_new_i2c_ctrl(&i2c_cfg);
}

/* Hardware boundary: instantiates the ES8311 speaker codec, ES7210 microphone
 * codec, their bus interfaces, and the external speaker amplifier. */
static esp_err_t create_codecs(void)
{
    const audio_codec_ctrl_if_t *speaker_ctrl = new_i2c_ctrl_if(ES8311_CODEC_DEFAULT_ADDR);
    const audio_codec_data_if_t *speaker_data = new_i2s_data_if();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(speaker_ctrl && speaker_data && gpio_if, ESP_FAIL, TAG, "speaker if");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = speaker_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
        .mclk_div = MCLK_MULTIPLE,
    };
    const audio_codec_if_t *speaker_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(speaker_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t speaker_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker_if,
        .data_if = speaker_data,
    };
    speaker_codec = esp_codec_dev_new(&speaker_dev);
    ESP_RETURN_ON_FALSE(speaker_codec, ESP_FAIL, TAG, "speaker dev");
    ESP_RETURN_ON_ERROR(enable_speaker_amp(), TAG, "speaker amp");

    const audio_codec_ctrl_if_t *mic_ctrl = new_i2c_ctrl_if(ES7210_CODEC_DEFAULT_ADDR);
    const audio_codec_data_if_t *mic_data = new_i2s_data_if();
    ESP_RETURN_ON_FALSE(mic_ctrl && mic_data, ESP_FAIL, TAG, "mic if");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = mic_ctrl,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = MCLK_MULTIPLE,
    };
    const audio_codec_if_t *mic_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(mic_if, ESP_FAIL, TAG, "es7210");

    esp_codec_dev_cfg_t mic_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = mic_if,
        .data_if = mic_data,
    };
    mic_codec = esp_codec_dev_new(&mic_dev);
    ESP_RETURN_ON_FALSE(mic_codec, ESP_FAIL, TAG, "mic dev");
    return ESP_OK;
}

/* Hardware boundary: opens/configures the speaker codec for the requested sample
 * rate, channel count, and output volume. */
static esp_err_t open_speaker(int sample_rate, int channels)
{
    if (speaker_open) {
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t cfg = {
        .bits_per_sample = AUDIO_BITS,
        .channel = channels,
        .channel_mask = channels == 1 ? 0x01 : 0x03,
        .sample_rate = sample_rate,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker_codec, &cfg) == ESP_CODEC_DEV_OK, ESP_FAIL,
                        TAG, "speaker open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(speaker_codec, SPEAKER_VOLUME) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker volume");
    speaker_open = true;
    return ESP_OK;
}

/* Hardware boundary: opens/configures the microphone codec, input gain, and
 * mute state for recording. */
static esp_err_t open_mic(int sample_rate)
{
    if (mic_open) {
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t cfg = {
        .bits_per_sample = AUDIO_BITS,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = sample_rate,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(mic_codec, &cfg) == ESP_CODEC_DEV_OK, ESP_FAIL,
                        TAG, "mic open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(mic_codec, 30.0f) == ESP_CODEC_DEV_OK, ESP_FAIL,
                        TAG, "mic gain");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_mute(mic_codec, false) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "mic unmute");
    mic_open = true;
    return ESP_OK;
}

/* Hardware boundary: safely retunes the shared I2S clock, closing codecs first
 * when the board switches between 16 kHz recording and 48 kHz playback. */
static esp_err_t reconfig_i2s_clock(int sample_rate)
{
    if (current_sample_rate == sample_rate) {
        return ESP_OK;
    }
    if (speaker_open) {
        esp_codec_dev_close(speaker_codec);
        speaker_open = false;
    }
    if (mic_open) {
        esp_codec_dev_close(mic_codec);
        mic_open = false;
    }

    if (i2s_channels_enabled) {
        esp_err_t err = i2s_channel_disable(tx_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(err, TAG, "disable tx");
        }
        err = i2s_channel_disable(rx_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(err, TAG, "disable rx");
        }
        i2s_channels_enabled = false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg), TAG, "tx clock");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(rx_handle, &clk_cfg), TAG, "rx clock");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_handle), TAG, "enable rx");
    i2s_channels_enabled = true;
    current_sample_rate = sample_rate;
    return ESP_OK;
}

/* Hardware boundary: initializes all audio buses, codecs, amplifier control, and
 * mutex protection, then leaves the board ready to record. */
esp_err_t audio_board_init(void)
{
    audio_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(audio_mutex, ESP_ERR_NO_MEM, TAG, "audio mutex");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(init_i2s(RECORD_SAMPLE_RATE), TAG, "i2s");
    ESP_RETURN_ON_ERROR(create_codecs(), TAG, "codecs");
    return audio_board_prepare_recording();
}

/* Hardware coordination helper: serializes access to shared audio hardware so
 * recording, playback, wake-word reads, and button replay do not collide. */
void audio_board_lock(void)
{
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
}

/* Hardware coordination helper: releases the shared audio-hardware lock. */
void audio_board_unlock(void)
{
    xSemaphoreGive(audio_mutex);
}

/* Hardware boundary: switches the codecs and I2S bus into 16 kHz microphone
 * recording mode while keeping the speaker available for tones. */
esp_err_t audio_board_prepare_recording(void)
{
    ESP_RETURN_ON_ERROR(reconfig_i2s_clock(RECORD_SAMPLE_RATE), TAG, "16k clock");
    ESP_RETURN_ON_ERROR(open_speaker(RECORD_SAMPLE_RATE, 2), TAG, "16k speaker");
    return open_mic(RECORD_SAMPLE_RATE);
}

/* Hardware boundary: switches the speaker path to the decoded media sample
 * rate/channel layout while leaving writes serialized by the audio mutex. */
esp_err_t audio_board_prepare_playback(int sample_rate, int channels)
{
    ESP_RETURN_ON_FALSE(sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid playback rate");
    ESP_RETURN_ON_FALSE(channels == 1 || channels == 2, ESP_ERR_INVALID_ARG, TAG,
                        "invalid playback channels");
    ESP_RETURN_ON_ERROR(reconfig_i2s_clock(sample_rate), TAG, "playback clock");
    return open_speaker(sample_rate, channels);
}

/* Hardware boundary: switches the speaker path to 48 kHz mono playback for
 * decoded Telegram replies and cached outgoing audio. */
esp_err_t audio_board_prepare_playback_48k(void)
{
    return audio_board_prepare_playback(PLAYBACK_SAMPLE_RATE, 1);
}

/* Hardware boundary: pulls interleaved stereo PCM frames directly from the
 * microphone codec. */
esp_err_t audio_board_read_stereo(int16_t *buffer, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(mic_open, ESP_ERR_INVALID_STATE, TAG, "mic closed");
    size_t bytes = frames * 2 * sizeof(int16_t);
    ESP_RETURN_ON_FALSE(esp_codec_dev_read(mic_codec, buffer, bytes) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "mic read");
    return ESP_OK;
}

/* Data preparation helper: limits a floating-point sample back into signed
 * 16-bit PCM range after software gain is applied. */
static int16_t clamp_i16(float sample)
{
    if (sample > 32767.0f) {
        return 32767;
    }
    if (sample < -32768.0f) {
        return -32768;
    }
    return (int16_t)sample;
}

/* Hardware plus data-prep boundary: reads stereo microphone samples from the
 * codec, mixes them down to mono, and applies software gain for wake/recording. */
esp_err_t audio_board_read_mono_gain(int16_t *buffer, size_t samples, float gain)
{
    int16_t *stereo = heap_caps_malloc(samples * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stereo) {
        stereo = malloc(samples * 2 * sizeof(int16_t));
    }
    ESP_RETURN_ON_FALSE(stereo, ESP_ERR_NO_MEM, TAG, "stereo buffer");
    esp_err_t err = audio_board_read_stereo(stereo, samples, 1000);
    if (err == ESP_OK) {
        for (size_t i = 0; i < samples; i++) {
            int32_t mixed = ((int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]) / 2;
            buffer[i] = clamp_i16((float)mixed * gain);
        }
    }
    free(stereo);
    return err;
}

/* Business-critical hardware workflow: records microphone audio until VAD sees
 * speech followed by silence, keeps preroll, and returns a WAV buffer plus
 * metrics for later Telegram upload. */
esp_err_t audio_board_record_until_silence_with_callback(recorded_audio_t *audio,
                                                         audio_board_record_chunk_cb_t cb,
                                                         void *ctx)
{
    memset(audio, 0, sizeof(*audio));
    ESP_RETURN_ON_ERROR(audio_board_prepare_recording(), TAG, "record mode");

    vad_handle_t vad = vad_create(VAD_MODE_1);
    ESP_RETURN_ON_FALSE(vad, ESP_ERR_NO_MEM, TAG, "vad create");

    size_t max_samples = RECORD_SAMPLE_RATE * MAX_RECORD_SECONDS;
    size_t max_data_bytes = max_samples * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(WAV_HEADER_BYTES + max_data_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = malloc(WAV_HEADER_BYTES + max_data_bytes);
    }
    if (!wav) {
        vad_destroy(vad);
        return ESP_ERR_NO_MEM;
    }

    size_t preroll_samples = (RECORD_SAMPLE_RATE * AUDIO_PREROLL_MS) / 1000;
    int16_t *preroll = heap_caps_malloc(preroll_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preroll) {
        preroll = malloc(preroll_samples * sizeof(int16_t));
    }
    if (!preroll) {
        vad_destroy(vad);
        free(wav);
        return ESP_ERR_NO_MEM;
    }

    int16_t read_mono[RECORD_VAD_FRAME_SAMPLES];
    int16_t *samples = (int16_t *)(wav + WAV_HEADER_BYTES);
    size_t written = 0;
    size_t preroll_write = 0;
    size_t preroll_count = 0;
    uint32_t silence_ms = 0;
    bool heard_voice = false;
    double sum_squares = 0;
    int64_t wait_start = esp_timer_get_time();
    esp_err_t err = ESP_OK;

    while (written < max_samples) {
        size_t chunk_samples = sizeof(read_mono) / sizeof(read_mono[0]);
        int64_t read_start = esp_timer_get_time();
        err = audio_board_read_mono_gain(read_mono, chunk_samples, 1.0f);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "record read failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (cb) {
            err = cb(read_mono, chunk_samples, ctx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "record callback failed: %s", esp_err_to_name(err));
                goto cleanup;
            }
        }
        uint32_t chunk_ms = (uint32_t)((esp_timer_get_time() - read_start) / 1000);
        if (chunk_ms < 1) {
            chunk_ms = (uint32_t)(chunk_samples * 1000 / RECORD_SAMPLE_RATE);
        }
        vad_state_t vad_state = vad_process(vad, read_mono, RECORD_SAMPLE_RATE, RECORD_VAD_FRAME_MS);

        if (!heard_voice) {
            for (size_t i = 0; i < chunk_samples; i++) {
                preroll[preroll_write] = read_mono[i];
                preroll_write = (preroll_write + 1) % preroll_samples;
                if (preroll_count < preroll_samples) {
                    preroll_count++;
                }
            }
            if (vad_state == VAD_SPEECH) {
                heard_voice = true;
                size_t start = preroll_count == preroll_samples ? preroll_write : 0;
                for (size_t i = 0; i < preroll_count && written < max_samples; i++) {
                    int16_t sample = preroll[(start + i) % preroll_samples];
                    samples[written++] = sample;
                    int16_t mag = sample == INT16_MIN ? INT16_MAX : abs(sample);
                    if (mag > audio->peak) {
                        audio->peak = mag;
                    }
                    sum_squares += (double)sample * (double)sample;
                }
            }
            if (!heard_voice && (esp_timer_get_time() - wait_start) / 1000 >= VOICE_WAIT_TIMEOUT_MS) {
                break;
            }
            continue;
        }

        for (size_t i = 0; i < chunk_samples && written < max_samples; i++) {
            int16_t sample = read_mono[i];
            samples[written++] = sample;
            int16_t mag = sample == INT16_MIN ? INT16_MAX : abs(sample);
            if (mag > audio->peak) {
                audio->peak = mag;
            }
            sum_squares += (double)sample * (double)sample;
        }

        uint32_t recorded_ms = (uint32_t)(written * 1000 / RECORD_SAMPLE_RATE);
        if (recorded_ms >= MIN_RECORD_MS && vad_state == VAD_SILENCE) {
            silence_ms += chunk_ms;
            if (silence_ms >= SILENCE_STOP_MS) {
                break;
            }
        } else {
            silence_ms = 0;
        }
    }

cleanup:
    vad_destroy(vad);
    free(preroll);
    if (err != ESP_OK) {
        free(wav);
        return err;
    }
    if (written == 0) {
        free(wav);
        return ESP_ERR_TIMEOUT;
    }

    uint32_t data_bytes = written * sizeof(int16_t);
    write_wav_header(wav, data_bytes);
    audio->ok = true;
    audio->wav = wav;
    audio->wav_size = WAV_HEADER_BYTES + data_bytes;
    audio->duration_ms = (uint32_t)(written * 1000 / RECORD_SAMPLE_RATE);
    audio->rms = (uint32_t)sqrt(sum_squares / written);
    return ESP_OK;
}

esp_err_t audio_board_record_until_silence(recorded_audio_t *audio)
{
    return audio_board_record_until_silence_with_callback(audio, NULL, NULL);
}

/* Hardware boundary: generates a sine wave and writes it to the speaker codec
 * for audible wake/done cues. */
esp_err_t audio_board_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    ESP_RETURN_ON_ERROR(audio_board_prepare_recording(), TAG, "tone mode");
    int16_t samples[128 * 2];
    uint32_t total_frames = RECORD_SAMPLE_RATE * duration_ms / 1000;
    float phase = 0;
    float step = 2.0f * (float)M_PI * (float)frequency_hz / (float)RECORD_SAMPLE_RATE;
    while (total_frames > 0) {
        uint32_t frames = total_frames > 128 ? 128 : total_frames;
        for (uint32_t i = 0; i < frames; i++) {
            int16_t sample = (int16_t)(sinf(phase) * 6000.0f);
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;
            phase += step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        ESP_RETURN_ON_FALSE(esp_codec_dev_write(speaker_codec, samples,
                                                frames * 2 * sizeof(int16_t)) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "tone write");
        total_frames -= frames;
    }
    return ESP_OK;
}

/* Hardware boundary: writes caller-provided mono PCM samples to the speaker
 * codec; decoded playback paths depend on this. */
esp_err_t audio_board_write_pcm(const int16_t *pcm, size_t samples)
{
    ESP_RETURN_ON_FALSE(speaker_open, ESP_ERR_INVALID_STATE, TAG, "speaker closed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_write(speaker_codec, (void *)pcm,
                                            samples * sizeof(int16_t)) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "pcm write");
    return ESP_OK;
}

/* Hardware boundary: reads the K1 button state through the TCA9555 expander. */
esp_err_t audio_board_read_k1(bool *pressed)
{
    ESP_RETURN_ON_FALSE(pressed, ESP_ERR_INVALID_ARG, TAG, "pressed missing");

    uint8_t input = 0xff;
    ESP_RETURN_ON_ERROR(tca_read(TCA9555_INPUT_PORT_1, &input), TAG, "key input read");
    *pressed = (input & (1U << KEY1_BIT)) == 0;
    return ESP_OK;
}

/* Hardware boundary: reads the K2 button state through the TCA9555 expander. */
esp_err_t audio_board_read_k2(bool *pressed)
{
    ESP_RETURN_ON_FALSE(pressed, ESP_ERR_INVALID_ARG, TAG, "pressed missing");

    uint8_t input = 0xff;
    ESP_RETURN_ON_ERROR(tca_read(TCA9555_INPUT_PORT_1, &input), TAG, "key input read");
    *pressed = (input & (1U << KEY2_BIT)) == 0;
    return ESP_OK;
}

/* Data ownership helper: frees the heap-backed WAV recording buffer after the
 * upload task has encoded or abandoned it. */
void audio_board_release_recording(recorded_audio_t *audio)
{
    if (audio && audio->wav) {
        free(audio->wav);
        memset(audio, 0, sizeof(*audio));
    }
}

/* Hardware access helper: exposes the speaker codec handle to playback code that
 * needs lower-level codec access. */
esp_codec_dev_handle_t audio_board_speaker_codec(void)
{
    return speaker_codec;
}
