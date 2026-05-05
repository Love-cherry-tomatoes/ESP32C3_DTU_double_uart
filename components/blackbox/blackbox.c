#include "blackbox.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "BLACKBOX";
/* 黑匣子分区按“头部 + 环形数据区”组织。 */
static const uint32_t BB_SECTOR_SIZE = 4096;
static const uint32_t BB_HEADER_SECTOR_COUNT = 1;
static const uint32_t BB_DATA_START_OFFSET = 4096;
static const uint32_t BB_ENTRIES_PER_SECTOR = BB_SECTOR_SIZE / BB_ENTRY_SIZE;

static const esp_partition_t *s_partition = NULL;
static blackbox_header_t s_header;
static SemaphoreHandle_t s_mutex;

static const char *s_cat_names[] = {
    "WIFI", "NET", "UART", "MQTT", "SYS", "OTA", "PT1K", "BLE"
};

static const char *s_level_names[] = {
    "INFO", "WARN", "ERR", "CRIT"
};

static uint32_t blackbox_capacity(void)
{
    /* 头部占用一个扇区，其余扇区全部用于存记录。 */
    uint32_t data_sectors = s_partition->size / BB_SECTOR_SIZE;
    if (data_sectors <= BB_HEADER_SECTOR_COUNT) {
        return 0;
    }
    data_sectors -= BB_HEADER_SECTOR_COUNT;
    return data_sectors * BB_ENTRIES_PER_SECTOR;
}

static uint32_t blackbox_entry_offset(uint32_t idx)
{
    /* 把逻辑记录索引映射到分区中的物理偏移。 */
    uint32_t sector_index = idx / BB_ENTRIES_PER_SECTOR;
    uint32_t sector_entry = idx % BB_ENTRIES_PER_SECTOR;
    return BB_DATA_START_OFFSET + sector_index * BB_SECTOR_SIZE + sector_entry * BB_ENTRY_SIZE;
}

static esp_err_t read_header(void)
{
    return esp_partition_read(s_partition, 0, &s_header, sizeof(blackbox_header_t));
}

static esp_err_t write_header(void)
{
    return esp_partition_write(s_partition, 0, &s_header, sizeof(blackbox_header_t));
}

static void erase_and_init(void)
{
    /* 黑匣子结构不匹配或首次使用时，整区擦除并重建头部。 */
    ESP_LOGI(TAG, "erasing blackbox partition...");
    esp_partition_erase_range(s_partition, 0, s_partition->size);

    uint32_t capacity = blackbox_capacity();
    s_header.magic = BB_MAGIC;
    s_header.write_index = 0;
    s_header.entry_count = 0;
    s_header.capacity = capacity;
    write_header();

    ESP_LOGI(TAG, "blackbox init: capacity=%" PRIu32 " entries", capacity);
}

esp_err_t blackbox_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "blackbox");
    if (!s_partition) {
        ESP_LOGE(TAG, "blackbox partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "blackbox partition: offset=0x%" PRIx32 " size=%" PRIu32 "KB",
             s_partition->address, (uint32_t)(s_partition->size / 1024));

    if (blackbox_capacity() == 0) {
        ESP_LOGE(TAG, "blackbox partition too small");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = read_header();
    /* magic 或容量不匹配通常意味着分区首次使用或版本变化。 */
    if (err != ESP_OK || s_header.magic != BB_MAGIC || s_header.capacity != blackbox_capacity()) {
        erase_and_init();
    } else {
        ESP_LOGI(TAG, "blackbox: %" PRIu32 " entries, write_index=%" PRIu32 ", capacity=%" PRIu32,
                 s_header.entry_count, s_header.write_index, s_header.capacity);
    }

    BB_INFO(BB_CAT_SYSTEM, BB_EVT_SYS_BOOT, 0, "boot");
    return ESP_OK;
}

esp_err_t blackbox_record(uint8_t category, uint8_t level, uint8_t event_id,
                           int32_t value, const char *msg)
{
    if (!s_partition) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    blackbox_entry_t entry = {
        .timestamp = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
        .category = category,
        .level = level,
        .event_id = event_id,
        .reserved = 0,
        .value = value,
    };
    memset(entry.msg, 0, sizeof(entry.msg));
    if (msg) {
        strncpy(entry.msg, msg, sizeof(entry.msg) - 1);
    }

    uint32_t idx = s_header.write_index;
    uint32_t sector_offset = blackbox_entry_offset(idx);
    uint32_t sector_entry = idx % BB_ENTRIES_PER_SECTOR;
    uint32_t sector_start = BB_DATA_START_OFFSET +
                            (idx / BB_ENTRIES_PER_SECTOR) * BB_SECTOR_SIZE;

    /* 每次写入新扇区第一条记录前先擦除整个扇区，符合 Flash 擦写约束。 */
    if (sector_entry == 0) {
        esp_partition_erase_range(s_partition, sector_start, BB_SECTOR_SIZE);
    }

    esp_err_t err = esp_partition_write(s_partition, sector_offset, &entry, BB_ENTRY_SIZE);
    if (err == ESP_OK) {
        s_header.write_index = (idx + 1) % s_header.capacity;
        s_header.entry_count++;
        write_header();
    }

    xSemaphoreGive(s_mutex);
    return err;
}

int blackbox_read_recent(blackbox_entry_t *entries, int max_count)
{
    /* recent 模式返回最近 N 条，适合诊断接口快速查看近期问题。 */
    if (!s_partition || s_header.entry_count == 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t total = s_header.entry_count;
    if (total > s_header.capacity) total = s_header.capacity;
    int count = (max_count < (int)total) ? max_count : (int)total;

    for (int i = 0; i < count; i++) {
        uint32_t idx;
        if (s_header.entry_count > s_header.capacity) {
            idx = (s_header.write_index + (total - count + i)) % s_header.capacity;
        } else {
            idx = total - count + i;
        }
        uint32_t offset = blackbox_entry_offset(idx);
        esp_partition_read(s_partition, offset, &entries[i], BB_ENTRY_SIZE);
    }

    xSemaphoreGive(s_mutex);
    return count;
}

int blackbox_read_all(blackbox_entry_t *entries, int max_count)
{
    /* all 模式按时间顺序输出当前环形缓冲区中的全部有效记录。 */
    if (!s_partition || s_header.entry_count == 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t total = s_header.entry_count;
    if (total > s_header.capacity) total = s_header.capacity;
    int count = (max_count < (int)total) ? max_count : (int)total;

    uint32_t start_idx = 0;
    if (s_header.entry_count > s_header.capacity) {
        start_idx = s_header.write_index;
    }

    for (int i = 0; i < count; i++) {
        uint32_t idx = (start_idx + i) % s_header.capacity;
        uint32_t offset = blackbox_entry_offset(idx);
        esp_partition_read(s_partition, offset, &entries[i], BB_ENTRY_SIZE);
    }

    xSemaphoreGive(s_mutex);
    return count;
}

void blackbox_clear(void)
{
    if (!s_partition) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    erase_and_init();
    xSemaphoreGive(s_mutex);
}

int blackbox_format_entry(const blackbox_entry_t *entry, char *buf, int buf_len)
{
    const char *cat = (entry->category < sizeof(s_cat_names) / sizeof(s_cat_names[0]))
                      ? s_cat_names[entry->category] : "UNK";
    const char *lvl = (entry->level < sizeof(s_level_names) / sizeof(s_level_names[0]))
                      ? s_level_names[entry->level] : "?";
    return snprintf(buf, buf_len, "[%" PRIu32 "s][%s][%s] evt=%u val=%" PRId32 " msg=%.16s",
                    entry->timestamp, cat, lvl, entry->event_id, entry->value, entry->msg);
}
