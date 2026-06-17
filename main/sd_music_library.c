#include "sd_music_library.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

static const char *TAG = "sd_music";

static sdmmc_card_t *card;
static sd_song_entry_t entries[SD_MAX_MANIFEST_ENTRIES];
static size_t entry_count;
static bool mounted;

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static void normalize_phrase(char *text)
{
    char *read = text;
    char *write = text;
    bool previous_space = true;

    while (*read) {
        unsigned char ch = (unsigned char)*read++;
        if (isalnum(ch)) {
            *write++ = (char)tolower(ch);
            previous_space = false;
        } else if (!previous_space) {
            *write++ = ' ';
            previous_space = true;
        }
    }
    if (write > text && write[-1] == ' ') {
        write--;
    }
    *write = '\0';
}

static void build_song_path(char *out, size_t out_size, const char *manifest_value)
{
    char name[SD_MAX_SONG_PATH_LEN] = {0};
    strlcpy(name, manifest_value, sizeof(name));
    char *song = trim(name);

    if (strchr(song, '/')) {
        snprintf(out, out_size, "%s/%s", SD_MOUNT_POINT, song[0] == '/' ? song + 1 : song);
    } else {
        snprintf(out, out_size, "%s/%s", SD_SONGS_DIR, song);
    }

    size_t len = strlen(out);
    if (len < 4 || strcasecmp(out + len - 4, ".mp3") != 0) {
        strlcat(out, ".mp3", out_size);
    }
}

static esp_err_t mount_sdcard(void)
{
    if (mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = SDMMC_BUS_WIDTH;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = SDMMC_CLK_IO;
    slot_config.cmd = SDMMC_CMD_IO;
    slot_config.d0 = SDMMC_D0_IO;
    slot_config.d1 = SDMMC_D1_IO;
    slot_config.d2 = SDMMC_D2_IO;
    slot_config.d3 = SDMMC_D3_IO;
#endif
    slot_config.cd = SDMMC_DET_IO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mounting SDMMC clk=%d cmd=%d d0=%d width=%d", SDMMC_CLK_IO, SDMMC_CMD_IO,
             SDMMC_D0_IO, SDMMC_BUS_WIDTH);
    ESP_RETURN_ON_ERROR(
        esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card),
        TAG, "mount %s", SD_MOUNT_POINT);
    mounted = true;
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

static esp_err_t load_manifest(void)
{
    FILE *file = fopen(SD_MANIFEST_PATH, "r");
    ESP_RETURN_ON_FALSE(file, ESP_ERR_NOT_FOUND, TAG, "open %s", SD_MANIFEST_PATH);

    entry_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), file) && entry_count < SD_MAX_MANIFEST_ENTRIES) {
        char *clean = trim(line);
        if (*clean == '\0' || *clean == '#') {
            continue;
        }

        char *sep = strchr(clean, '|');
        if (!sep) {
            ESP_LOGW(TAG, "skipping manifest line without pipe: %s", clean);
            continue;
        }
        *sep = '\0';
        char *phrase = trim(clean);
        char *song = trim(sep + 1);
        normalize_phrase(phrase);
        if (*phrase == '\0' || *song == '\0') {
            continue;
        }

        sd_song_entry_t *entry = &entries[entry_count];
        entry->command_id = (int)entry_count + 1;
        strlcpy(entry->phrase, phrase, sizeof(entry->phrase));
        build_song_path(entry->song_path, sizeof(entry->song_path), song);
        ESP_LOGI(TAG, "command %d: \"%s\" -> %s", entry->command_id, entry->phrase,
                 entry->song_path);
        entry_count++;
    }
    fclose(file);

    ESP_RETURN_ON_FALSE(entry_count > 0, ESP_ERR_NOT_FOUND, TAG, "manifest has no songs");
    return ESP_OK;
}

esp_err_t sd_music_library_init(void)
{
    ESP_RETURN_ON_ERROR(mount_sdcard(), TAG, "sdcard");
    return load_manifest();
}

esp_err_t sd_music_library_mount(void)
{
    return mount_sdcard();
}

bool sd_music_library_ready(void)
{
    return mounted && entry_count > 0;
}

size_t sd_music_library_entry_count(void)
{
    return entry_count;
}

const sd_song_entry_t *sd_music_library_entry_at(size_t index)
{
    return index < entry_count ? &entries[index] : NULL;
}

const sd_song_entry_t *sd_music_library_find_command(int command_id)
{
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].command_id == command_id) {
            return &entries[i];
        }
    }
    return NULL;
}
