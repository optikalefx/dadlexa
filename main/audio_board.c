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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_PORT I2C_NUM_0
#define I2S_PORT I2S_NUM_0
#define MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define TCA9555_ADDR 0x20
#define TCA9555_OUTPUT_PORT_1 0x03
#define TCA9555_CONFIG_PORT_1 0x07
#define SPEAKER_PA_BIT 0
#define WAV_HEADER_BYTES 44

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

static void write_le16(uint8_t *target, uint16_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
}

static void write_le32(uint8_t *target, uint32_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
    target[2] = (value >> 16) & 0xff;
    target[3] = (value >> 24) & 0xff;
}

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

static esp_err_t tca_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(tca_handle, &reg, 1, value, 1, pdMS_TO_TICKS(1000));
}

static esp_err_t tca_write(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(tca_handle, payload, sizeof(payload), pdMS_TO_TICKS(1000));
}

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

static const audio_codec_ctrl_if_t *new_i2c_ctrl_if(uint8_t addr)
{
    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_PORT, .addr = addr, .bus_handle = i2c_bus};
    return audio_codec_new_i2c_ctrl(&i2c_cfg);
}

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

static esp_err_t reconfig_i2s_clock(int sample_rate)
{
    if (current_sample_rate == sample_rate) {
        return ESP_OK;
    }
    bool codec_close_should_disable_channels = speaker_open || mic_open;
    if (speaker_open) {
        esp_codec_dev_close(speaker_codec);
        speaker_open = false;
    }
    if (mic_open) {
        esp_codec_dev_close(mic_codec);
        mic_open = false;
    }

    if (codec_close_should_disable_channels) {
        i2s_channels_enabled = false;
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

esp_err_t audio_board_init(void)
{
    audio_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(audio_mutex, ESP_ERR_NO_MEM, TAG, "audio mutex");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(init_i2s(RECORD_SAMPLE_RATE), TAG, "i2s");
    ESP_RETURN_ON_ERROR(create_codecs(), TAG, "codecs");
    return audio_board_prepare_recording();
}

void audio_board_lock(void)
{
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
}

void audio_board_unlock(void)
{
    xSemaphoreGive(audio_mutex);
}

esp_err_t audio_board_prepare_recording(void)
{
    ESP_RETURN_ON_ERROR(reconfig_i2s_clock(RECORD_SAMPLE_RATE), TAG, "16k clock");
    ESP_RETURN_ON_ERROR(open_speaker(RECORD_SAMPLE_RATE, 2), TAG, "16k speaker");
    return open_mic(RECORD_SAMPLE_RATE);
}

esp_err_t audio_board_prepare_playback_48k(void)
{
    ESP_RETURN_ON_ERROR(reconfig_i2s_clock(PLAYBACK_SAMPLE_RATE), TAG, "48k clock");
    return open_speaker(PLAYBACK_SAMPLE_RATE, 1);
}

esp_err_t audio_board_read_stereo(int16_t *buffer, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(mic_open, ESP_ERR_INVALID_STATE, TAG, "mic closed");
    size_t bytes = frames * 2 * sizeof(int16_t);
    ESP_RETURN_ON_FALSE(esp_codec_dev_read(mic_codec, buffer, bytes) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "mic read");
    return ESP_OK;
}

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

static uint32_t calculate_rms(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    double sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (double)samples[i] * (double)samples[i];
    }
    return (uint32_t)sqrt(sum / count);
}

esp_err_t audio_board_record_until_silence(recorded_audio_t *audio)
{
    memset(audio, 0, sizeof(*audio));
    ESP_RETURN_ON_ERROR(audio_board_prepare_recording(), TAG, "record mode");

    size_t max_samples = RECORD_SAMPLE_RATE * MAX_RECORD_SECONDS;
    size_t max_data_bytes = max_samples * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(WAV_HEADER_BYTES + max_data_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = malloc(WAV_HEADER_BYTES + max_data_bytes);
    }
    ESP_RETURN_ON_FALSE(wav, ESP_ERR_NO_MEM, TAG, "wav alloc");

    size_t preroll_samples = (RECORD_SAMPLE_RATE * AUDIO_PREROLL_MS) / 1000;
    int16_t *preroll = heap_caps_malloc(preroll_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preroll) {
        preroll = malloc(preroll_samples * sizeof(int16_t));
    }
    if (!preroll) {
        free(wav);
        return ESP_ERR_NO_MEM;
    }

    int16_t read_mono[AUDIO_READ_BUFFER_BYTES / (sizeof(int16_t) * 2)];
    int16_t *samples = (int16_t *)(wav + WAV_HEADER_BYTES);
    size_t written = 0;
    size_t preroll_write = 0;
    size_t preroll_count = 0;
    uint32_t silence_ms = 0;
    bool heard_voice = false;
    uint8_t voice_confirm = 0;
    double sum_squares = 0;
    uint32_t noise_floor = UINT32_MAX;
    uint32_t start_threshold = VOICE_START_RMS_THRESHOLD;
    uint32_t silence_threshold = SILENCE_RMS_THRESHOLD;
    int64_t wait_start = esp_timer_get_time();

    while (written < max_samples) {
        size_t chunk_samples = sizeof(read_mono) / sizeof(read_mono[0]);
        int64_t read_start = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(audio_board_read_mono_gain(read_mono, chunk_samples, 1.0f), TAG,
                            "record read");
        uint32_t chunk_ms = (uint32_t)((esp_timer_get_time() - read_start) / 1000);
        if (chunk_ms < 1) {
            chunk_ms = (uint32_t)(chunk_samples * 1000 / RECORD_SAMPLE_RATE);
        }

        uint32_t chunk_rms = calculate_rms(read_mono, chunk_samples);
        if (!heard_voice) {
            if (chunk_rms < noise_floor) {
                noise_floor = chunk_rms;
                start_threshold = noise_floor + VOICE_START_RMS_MARGIN;
                if (start_threshold < VOICE_START_RMS_THRESHOLD) {
                    start_threshold = VOICE_START_RMS_THRESHOLD;
                }
                silence_threshold = noise_floor + SILENCE_RMS_MARGIN;
                if (silence_threshold < SILENCE_RMS_THRESHOLD) {
                    silence_threshold = SILENCE_RMS_THRESHOLD;
                }
                uint32_t max_silence_threshold = start_threshold > 25 ? start_threshold - 25 : start_threshold;
                if (silence_threshold > max_silence_threshold) {
                    silence_threshold = max_silence_threshold;
                }
            }
            for (size_t i = 0; i < chunk_samples; i++) {
                preroll[preroll_write] = read_mono[i];
                preroll_write = (preroll_write + 1) % preroll_samples;
                if (preroll_count < preroll_samples) {
                    preroll_count++;
                }
            }
            if (chunk_rms >= start_threshold) {
                voice_confirm++;
                if (voice_confirm >= VOICE_START_CONFIRM_CHUNKS) {
                    heard_voice = true;
                    size_t start = preroll_count == preroll_samples ? preroll_write : 0;
                    for (size_t i = 0; i < preroll_count && written < max_samples; i++) {
                        samples[written++] = preroll[(start + i) % preroll_samples];
                    }
                }
            } else {
                voice_confirm = 0;
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
        if (recorded_ms >= MIN_RECORD_MS && chunk_rms < silence_threshold) {
            silence_ms += chunk_ms;
            if (silence_ms >= SILENCE_STOP_MS) {
                break;
            }
        } else {
            silence_ms = 0;
        }
    }

    free(preroll);
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

esp_err_t audio_board_write_pcm(const int16_t *pcm, size_t samples)
{
    ESP_RETURN_ON_FALSE(speaker_open, ESP_ERR_INVALID_STATE, TAG, "speaker closed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_write(speaker_codec, (void *)pcm,
                                            samples * sizeof(int16_t)) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "pcm write");
    return ESP_OK;
}

void audio_board_release_recording(recorded_audio_t *audio)
{
    if (audio && audio->wav) {
        free(audio->wav);
        memset(audio, 0, sizeof(*audio));
    }
}

esp_codec_dev_handle_t audio_board_speaker_codec(void)
{
    return speaker_codec;
}
