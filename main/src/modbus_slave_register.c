#include "modbus_slave_register.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "MB_BAR_FILE";

#define BAR_FILE_MAGIC "HMI_BAR_RANGES_V2"
#define BAR_FILE_VALUE_COUNT \
    ((HOLD_ADDR_BAR_PF_MAX - HOLD_ADDR_BAR_L12V_MIN) + 1U)

static bool parse_u16_line(const char *line, uint16_t *value)
{
    if (line == NULL || value == NULL)
        return false;

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(line, &end, 10);

    if (errno != 0 || end == line || parsed > UINT16_MAX)
        return false;

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;

    if (*end != '\0')
        return false;

    *value = (uint16_t)parsed;
    return true;
}

static bool validate_pair(uint16_t minimum, uint16_t maximum)
{
    return minimum < maximum;
}

static bool validate_bar_values(const uint16_t values[BAR_FILE_VALUE_COUNT])
{
    /* Values map sequentially to holding registers 17..38. */
    for (uint16_t i = 0U; i < BAR_FILE_VALUE_COUNT; i += 2U)
    {
        if (!validate_pair(values[i], values[i + 1U]))
            return false;
    }

    /* Additional sanity checks for this project's raw engineering scales. */
    if (values[0] > 10000U || values[1] > 10000U) /* L12 voltage */
        return false;

    if (values[16] > 10000U || values[17] > 10000U) /* frequency */
        return false;

    if (values[20] > 1000U || values[21] > 2000U) /* PF */
        return false;

    return true;
}

bool modbus_slave_load_bar_ranges_from_file(void)
{
    FILE *fp = fopen(HOLD_REG_FILE_PATH, "r");
    if (fp == NULL)
    {
        ESP_LOGI(TAG, "No bar-range file; using compiled defaults");
        return false;
    }

    char line[64];
    if (fgets(line, sizeof(line), fp) == NULL)
    {
        fclose(fp);
        ESP_LOGW(TAG, "Empty bar-range file; using compiled defaults");
        return false;
    }

    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, BAR_FILE_MAGIC) != 0)
    {
        fclose(fp);
        ESP_LOGW(TAG,
                 "Ignoring legacy bar-range file without %s header; defaults retained",
                 BAR_FILE_MAGIC);
        return false;
    }

    uint16_t values[BAR_FILE_VALUE_COUNT] = {0};
    uint16_t count = 0U;

    while (count < BAR_FILE_VALUE_COUNT && fgets(line, sizeof(line), fp) != NULL)
    {
        if (!parse_u16_line(line, &values[count]))
        {
            fclose(fp);
            ESP_LOGW(TAG, "Invalid value at V2 bar-range line %u", (unsigned)(count + 2U));
            return false;
        }
        ++count;
    }

    fclose(fp);

    if (count != BAR_FILE_VALUE_COUNT)
    {
        ESP_LOGW(TAG,
                 "Incomplete V2 bar-range file: expected=%u read=%u; defaults retained",
                 (unsigned)BAR_FILE_VALUE_COUNT,
                 (unsigned)count);
        return false;
    }

    if (!validate_bar_values(values))
    {
        ESP_LOGW(TAG, "Rejected invalid V2 bar ranges; defaults retained");
        return false;
    }

    for (uint16_t i = 0U; i < BAR_FILE_VALUE_COUNT; ++i)
    {
        slave_holding_regs[HOLD_ADDR_BAR_L12V_MIN + i] = values[i];
    }

    ESP_LOGI(TAG,
             "Loaded validated V2 bar ranges: HR17=%u HR18=%u",
             slave_holding_regs[HOLD_ADDR_BAR_L12V_MIN],
             slave_holding_regs[HOLD_ADDR_BAR_L12V_MAX]);

    return true;
}
