#pragma once

#include <stddef.h>
#include <stdint.h>

#define LED_GPIO 38
#define LED_COUNT 7
#define LED_BRIGHTNESS 40

#define I2C_SDA_IO 11
#define I2C_SCL_IO 10
#define I2S_MCLK_IO 12
#define I2S_BCLK_IO 13
#define I2S_WS_IO 14
#define I2S_DIN_IO 15
#define I2S_DOUT_IO 16

#define RECORD_SAMPLE_RATE 16000
#define PLAYBACK_SAMPLE_RATE 48000
#define AUDIO_BITS 16
#define SPEAKER_VOLUME 65

#define WAKE_MODEL_HINT "hiesp"
#define MIC_SOFTWARE_GAIN 12.0f

#define SD_MOUNT_POINT "/sdcard"
#define SD_MANIFEST_PATH SD_MOUNT_POINT "/manifest.txt"
#define SD_SONGS_DIR SD_MOUNT_POINT "/songs"
#define SD_MAX_MANIFEST_ENTRIES 64
#define SD_MAX_PHRASE_LEN 63
#define SD_MAX_SONG_PATH_LEN 160
#define SDMMC_BUS_WIDTH 1
#define SDMMC_CLK_IO 40
#define SDMMC_CMD_IO 42
#define SDMMC_D0_IO 41
#define SDMMC_D1_IO -1
#define SDMMC_D2_IO -1
#define SDMMC_D3_IO -1
#define SDMMC_DET_IO -1
#define VOICE_COMMAND_TIMEOUT_MS 6000
#define MULTINET_MODEL_HINT "en"

#define SILENCE_STOP_MS 2000
#define MIN_RECORD_MS 1500
#define VOICE_WAIT_TIMEOUT_MS 10000
#define MAX_RECORD_SECONDS 20
#define AUDIO_PREROLL_MS 1000

#define TELEGRAM_REPLY_POLL_INTERVAL_MS 5000
#define TELEGRAM_REPLY_WAIT_TIMEOUT_MS 300000
#define TELEGRAM_REPLY_MAX_DOWNLOAD_BYTES (1024 * 1024)

typedef struct {
    const char *wifi_ssid;
    const char *wifi_password;
    const char *telegram_bot_token;
    const char *telegram_chat_id;
} app_config_t;

const app_config_t *app_config_get(void);
