#include "app_config.h"

#include "../arduino_secrets.h"

static const app_config_t config = {
    .wifi_ssid = WIFI_SSID,
    .wifi_password = WIFI_PASSWORD,
    .telegram_bot_token = TELEGRAM_BOT_TOKEN,
    .telegram_chat_id = TELEGRAM_CHAT_ID,
};

const app_config_t *app_config_get(void)
{
    return &config;
}
