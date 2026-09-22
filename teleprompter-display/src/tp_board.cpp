#include "tp_board.h"

#include <Arduino.h>
#include <driver/i2c_master.h>
#include <esp_log.h>

#include "tp_config.h"

static const char *TAG = "tp_board";

// Expander registers
#define IOEXP_REG_MODE      0x02
#define IOEXP_REG_OUTPUT    0x03
#define IOEXP_REG_INPUT     0x04
#define IOEXP_REG_PWM       0x05

// Expander pins
#define IOEXP_TOUCH_RST     1
#define IOEXP_BACKLIGHT     2
#define IOEXP_LCD_RST       3

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static uint8_t s_out_state = 0xFF;

static void exp_write(uint8_t reg, uint8_t value)
{
    if (!s_dev) return;
    const uint8_t frame[2] = { reg, value };
    i2c_master_transmit(s_dev, frame, sizeof(frame), 100);
}

static void exp_set_pin(uint8_t pin, bool high)
{
    if (high) s_out_state |= (uint8_t)(1u << pin);
    else      s_out_state &= (uint8_t)~(1u << pin);
    exp_write(IOEXP_REG_OUTPUT, s_out_state);
}

bool tp_board_init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t)TP_PIN_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)TP_PIN_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;

    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TP_IO_EXPANDER_ADDR;
    dev_cfg.scl_speed_hz = TP_I2C_FREQ_HZ;

    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not reachable at 0x%02X", TP_IO_EXPANDER_ADDR);
        return false;
    }

    // All expander pins to output, everything high except the backlight, so the
    // panel never shows a frame of garbage while it is still starting up.
    exp_write(IOEXP_REG_MODE, 0xFF);
    s_out_state = (uint8_t)(0xFF & ~(1u << IOEXP_BACKLIGHT));
    exp_write(IOEXP_REG_OUTPUT, s_out_state);
    delay(10);

    // Vendor power-up sequence, kept intact.
    pinMode(TP_PIN_TOUCH_INT, OUTPUT);
    exp_set_pin(IOEXP_TOUCH_RST, false);
    delay(100);
    digitalWrite(TP_PIN_TOUCH_INT, LOW);
    delay(100);
    exp_set_pin(IOEXP_TOUCH_RST, true);
    delay(200);

    ESP_LOGI(TAG, "IO expander ready");
    return true;
}

void tp_board_backlight(bool on)
{
    exp_set_pin(IOEXP_BACKLIGHT, on);
}

// The expander's PWM register is INVERTED: 0 is full brightness and 255 is off.
// That is also the explanation for the odd clamp in Waveshare's own driver,
// which capped its input at 97 and called it "prevent the screen from
// completely turning off" - only meaningful if a bigger number means darker.
void tp_board_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 10) percent = 10;         // never let it go fully dark
    exp_write(IOEXP_REG_PWM, (uint8_t)(((100 - (int)percent) * 255) / 100));
}
