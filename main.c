#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#define IR1_PIN GPIO_NUM_18
#define IR2_PIN GPIO_NUM_19

#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22

#define I2C_PORT I2C_NUM_0
#define LCD_ADDRESS 0x27

#define SENSOR_ACTIVE 0
#define DEBOUNCE_TIME 80
#define TIMEOUT 2000

int count = 0;

typedef enum
{
    IDLE,
    IR1_FIRST,
    IR2_FIRST,
    WAIT_CLEAR
} State;

State state = IDLE;
TickType_t state_time = 0;

/* ---------- LCD ---------- */

void lcd_write(unsigned char data)
{
    i2c_master_write_to_device(
        I2C_PORT,
        LCD_ADDRESS,
        &data,
        1,
        pdMS_TO_TICKS(100)
    );
}

void lcd_enable(unsigned char data)
{
    lcd_write(data | 0x04);
    esp_rom_delay_us(1);

    lcd_write(data & ~0x04);
    esp_rom_delay_us(50);
}

void lcd_nibble(unsigned char data, unsigned char rs)
{
    data &= 0xF0;

    if (rs)
        data |= 0x01;

    data |= 0x08;

    lcd_enable(data);
}

void lcd_byte(unsigned char data, unsigned char rs)
{
    lcd_nibble(data & 0xF0, rs);
    lcd_nibble((data << 4) & 0xF0, rs);
}

void lcd_command(unsigned char command)
{
    lcd_byte(command, 0);

    if (command == 0x01 || command == 0x02)
        vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_print(const char *text)
{
    while (*text)
    {
        lcd_byte(*text, 1);
        text++;
    }
}

void lcd_cursor(int row, int column)
{
    int address = (row == 0) ? column : 0x40 + column;
    lcd_command(0x80 + address);
}

void lcd_init()
{
    vTaskDelay(pdMS_TO_TICKS(50));

    lcd_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_nibble(0x20, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x06);
    lcd_command(0x01);
}

/* ---------- I2C ---------- */

void i2c_init()
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &config));

    ESP_ERROR_CHECK(
        i2c_driver_install(
            I2C_PORT,
            I2C_MODE_MASTER,
            0,
            0,
            0
        )
    );
}

/* ---------- LCD UPDATE ---------- */

void update_lcd()
{
    char text[17];

    lcd_command(0x01);

    lcd_cursor(0, 0);

    snprintf(text, sizeof(text), "COUNT: %d", count);
    lcd_print(text);

    lcd_cursor(1, 0);

    if (count == 0)
        lcd_print("STATUS: EMPTY");
    else
        lcd_print("STATUS: ACTIVE");
}

/* ---------- PEOPLE COUNTER ---------- */

void people_counter(void *arg)
{
    while (1)
    {
        int ir1 = (gpio_get_level(IR1_PIN) == SENSOR_ACTIVE);
        int ir2 = (gpio_get_level(IR2_PIN) == SENSOR_ACTIVE);

        TickType_t now = xTaskGetTickCount();

        if (state == IDLE)
        {
            if (ir1 && !ir2)
            {
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

                if (gpio_get_level(IR1_PIN) == SENSOR_ACTIVE)
                {
                    state = IR1_FIRST;
                    state_time = xTaskGetTickCount();
                    ESP_LOGI("COUNTER", "IR1 first");
                }
            }
            else if (ir2 && !ir1)
            {
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

                if (gpio_get_level(IR2_PIN) == SENSOR_ACTIVE)
                {
                    state = IR2_FIRST;
                    state_time = xTaskGetTickCount();
                    ESP_LOGI("COUNTER", "IR2 first");
                }
            }
        }

        else if (state == IR1_FIRST)
        {
            if (ir2)
            {
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

                if (gpio_get_level(IR2_PIN) == SENSOR_ACTIVE)
                {
                    count++;

                    ESP_LOGI("COUNTER", "ENTRY: %d", count);

                    update_lcd();

                    state = WAIT_CLEAR;
                }
            }
            else if (now - state_time > pdMS_TO_TICKS(TIMEOUT))
            {
                state = IDLE;
            }
        }

        else if (state == IR2_FIRST)
        {
            if (ir1)
            {
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

                if (gpio_get_level(IR1_PIN) == SENSOR_ACTIVE)
                {
                    if (count > 0)
                        count--;

                    ESP_LOGI("COUNTER", "EXIT: %d", count);

                    update_lcd();

                    state = WAIT_CLEAR;
                }
            }
            else if (now - state_time > pdMS_TO_TICKS(TIMEOUT))
            {
                state = IDLE;
            }
        }

        else if (state == WAIT_CLEAR)
        {
            if (!ir1 && !ir2)
            {
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

                if (gpio_get_level(IR1_PIN) != SENSOR_ACTIVE &&
                    gpio_get_level(IR2_PIN) != SENSOR_ACTIVE)
                {
                    state = IDLE;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---------- MAIN ---------- */

void app_main()
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << IR1_PIN) | (1ULL << IR2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    i2c_init();
    lcd_init();

    lcd_cursor(0, 0);
    lcd_print("PEOPLE COUNTER");

    lcd_cursor(1, 0);
    lcd_print("Starting...");

    vTaskDelay(pdMS_TO_TICKS(1500));

    update_lcd();

    xTaskCreate(
        people_counter,
        "counter",
        4096,
        NULL,
        5,
        NULL
    );

    ESP_LOGI("COUNTER", "System ready");
}