/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Espressif ESP-VoCat, first sold as EchoEar (喵伴): a round 1.85" 360 px
 * ST77916 LCD on QSPI with CST816S touch, an ES8311 speaker codec driving an
 * NS4150B 3 W amp, an ES7210 with two mics, a BQ27220 fuel gauge, a BMI270
 * IMU (unused), two capacitive touch pads on top of the head, and BOOT and
 * RST buttons on the base. POWER is a hardware switch (SAM8108) the ESP32
 * can't see.
 *
 * Two revisions are out there, told apart by the silkscreen on the main board:
 * v1.2 (ESP32-S3-WROOM-1-N16R16VA: 16 MB quad flash, 16 MB octal PSRAM) and
 * v1.0 (ESP32-S3-WROOM-2-N32R16V: 32 MB octal flash, 16 MB octal PSRAM). The
 * flash mode is found at boot either way; a few pins moved:
 *
 *                 v1.0             v1.2
 *   I2S DIN       GPIO15           GPIO3
 *   amp enable    GPIO4            GPIO15
 *   panel reset   GPIO3, low       GPIO47, high
 *   head pads     GPIO7            GPIO7 and GPIO6
 *   codec power   -                GPIO48, high is on
 *
 * Pins are from Espressif's BSP (esp-bsp bsp/esp_vocat, v1.2), the esp-dev-kits
 * EchoEar v1.0 and v1.2 user guides, and xiaozhi-esp32's esp-vocat board
 * (main/boards/espressif/esp-vocat), which runs on both and tells them apart
 * the way detect_rev() does. BOOT talks, as on xiaozhi; so does a hand on the
 * head (CONFIG_MUSE_VOCAT_HEAD_TALK).
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/touch_sens.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espressif_vocat_lcd_init.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_18
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_13
#define LCD_D2 GPIO_NUM_11
#define LCD_D3 GPIO_NUM_12
#define LCD_CS GPIO_NUM_14
#define LCD_BL GPIO_NUM_44         /* backlight, PWM, high is on */
#define LCD_POWER GPIO_NUM_9       /* low powers the panel and the SD card */
#define DRAW_BUF_LINES 90          /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define BL_TIMER LEDC_TIMER_1
#define BL_CHANNEL LEDC_CHANNEL_1

#define I2C_SDA GPIO_NUM_2
#define I2C_SCL GPIO_NUM_1
#define I2S_MCLK GPIO_NUM_42
#define I2S_BCLK GPIO_NUM_40
#define I2S_WS GPIO_NUM_39
#define I2S_DOUT GPIO_NUM_41
#define CODEC_POWER GPIO_NUM_48    /* v1.2: high switches on the codecs' rail */

#define TP_INT GPIO_NUM_10
#define BOOT_GPIO GPIO_NUM_0
#define LED_GPIO GPIO_NUM_43       /* green LED on the mic board, low is on */

#define ES8311_ADDR 0x18           /* 7-bit; esp_codec_dev takes the 8-bit form */
#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define GAUGE_ADDR 0x55            /* BQ27220 */
#define GAUGE_VOLTAGE 0x08         /* mV */
#define GAUGE_STATUS 0x0A          /* BatteryStatus; bit 0 is DSG, discharging */
#define GAUGE_CURRENT 0x0C         /* mA, signed, positive while charging */
#define GAUGE_SOC 0x2C             /* % */
#define GAUGE_DSG BIT(0)
#define CHARGING_MA 30             /* xiaozhi's threshold: AverageCurrent lags, Current doesn't */

/* The two head pads' touch channels (touch channel n is GPIOn on the S3). */
#define HEAD_PAD_A 7
#define HEAD_PAD_B 6               /* v1.2 only */
#define HEAD_DEBOUNCE 2            /* polls (10 ms) a change has to last */
#define HEAD_STUCK_POLLS 6000      /* a minute held: a shifted level, not a hand */

typedef struct {
    const char *name;
    gpio_num_t i2s_din, amp, lcd_rst;
    bool lcd_rst_high;
    int pads;
} vocat_rev_t;

static const vocat_rev_t REV_1_0 = { "v1.0", GPIO_NUM_15, GPIO_NUM_4, GPIO_NUM_3, false, 1 };
static const vocat_rev_t REV_1_2 = { "v1.2", GPIO_NUM_3, GPIO_NUM_15, GPIO_NUM_47, true, 2 };

/*
 * The revision found at power-on, kept across resets that don't cut the
 * power. After a reset the codec rail on v1.2 stays charged for a while, so
 * the codec answers with its switch off and v1.2 would look like v1.0. xiaozhi
 * learned this the hard way (its issue #1202) and only probes at power-on.
 */
#define REV_MAGIC 0x31434F56u      /* "VOC1" */
static RTC_NOINIT_ATTR uint32_t s_rev_magic;
static RTC_NOINIT_ATTR uint8_t s_rev_v12;

static const vocat_rev_t *s_rev = &REV_1_2;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp, s_gauge;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_codec_dev_handle_t s_spk, s_mic;
static muse_gpio_button_t s_boot;
static bool s_talk_down;
static bool s_gauge_ok;
static volatile bool s_tp_irq;
static bool s_tp_down;

static void drive(gpio_num_t gpio, int level)
{
    /* Latch the level first, so the pin never glitches to the other one. */
    gpio_set_level(gpio, level);
    const gpio_config_t io = { .pin_bit_mask = BIT64(gpio), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
}

static bool codec_answers(void)
{
    return i2c_master_probe(s_i2c, ES8311_ADDR, 100) == ESP_OK;
}

/* The rail (an SY8088) and the ES8311 take a while after a cold start. */
static bool wait_for_codec(void)
{
    for (int i = 0; i < 10; i++) {
        if (codec_answers()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

/* The revision chosen in menuconfig, or found at an earlier power-on; NULL if it has to be found. */
static const vocat_rev_t *known_rev(void)
{
#if CONFIG_MUSE_VOCAT_REV_1_0
    return &REV_1_0;
#elif CONFIG_MUSE_VOCAT_REV_1_2
    return &REV_1_2;
#else
    if (esp_reset_reason() != ESP_RST_POWERON && s_rev_magic == REV_MAGIC && s_rev_v12 <= 1) {
        return s_rev_v12 ? &REV_1_2 : &REV_1_0;
    }
    return NULL;
#endif
}

/*
 * v1.2 powers the codecs from a rail GPIO48 switches; v1.0 powers them all the
 * time and leaves GPIO48 unconnected. So with GPIO48 low, only v1.0's codec
 * answers. Unpowered codecs load the shared I2C bus, so GPIO48 goes high
 * whenever the answer is v1.2 or unknown.
 */
static const vocat_rev_t *detect_rev(void)
{
    const vocat_rev_t *known = known_rev();
    if (known == &REV_1_2) {
        drive(CODEC_POWER, 1);
        if (!wait_for_codec()) {
            ESP_LOGW(TAG, "speaker codec not answering");
        }
    }
    if (known) {
        return known;
    }
    drive(CODEC_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(200));   /* xiaozhi waits 100 ms; the rail has to drain */
    if (codec_answers()) {
        s_rev_v12 = 0;
        s_rev_magic = REV_MAGIC;
        return &REV_1_0;
    }
    gpio_set_level(CODEC_POWER, 1);
    if (!wait_for_codec()) {
        /* Not remembered, so the next power-on asks again. */
        ESP_LOGE(TAG, "speaker codec not answering either way; assuming v1.2");
        return &REV_1_2;
    }
    s_rev_v12 = 1;
    s_rev_magic = REV_MAGIC;
    return &REV_1_2;
}

static esp_err_t add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, dev);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
}

/* ---------- Head touch pads ---------- */

#if CONFIG_MUSE_VOCAT_HEAD_TALK
static touch_sensor_handle_t s_touch;
static touch_channel_handle_t s_pad[2];
static uint32_t s_pad_base[2];   /* untouched level */
static int s_pads;

static uint32_t pad_read(int i)
{
    uint32_t v[TOUCH_SAMPLE_CFG_NUM] = { 0 };
    return touch_channel_read_data(s_pad[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, v) == ESP_OK ? v[0] : 0;
}

/*
 * Espressif's BSP setup for these pads: one sample configuration, charged 500
 * times between 0.5 and 2.2 V at the fastest speed, with the default filters.
 * Its relative threshold is the default here too. The controller's own
 * active flag isn't used: its threshold is in counts, which vary from pad to
 * pad and board to board.
 */
static esp_err_t head_init(void)
{
    touch_sensor_sample_config_t sample[] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    const touch_sensor_config_t sens = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample);
    ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&sens, &s_touch), TAG, "touch controller");
    const touch_channel_config_t chan = {
        .active_thresh = { 2000 },
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    const int ids[2] = { HEAD_PAD_A, HEAD_PAD_B };
    s_pads = s_rev->pads;
    for (int i = 0; i < s_pads; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_new_channel(s_touch, ids[i], &chan, &s_pad[i]), TAG, "touch pad %d", ids[i]);
    }
    const touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_touch, &filter), TAG, "touch filter");
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "touch enable");
    /* A few single scans to settle the filtered readings, as the BSP does. */
    for (int i = 0; i < 3; i++) {
        touch_sensor_trigger_oneshot_scanning(s_touch, 2000);
    }
    for (int i = 0; i < s_pads; i++) {
        s_pad_base[i] = pad_read(i);
    }
    ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s_touch), TAG, "touch scanning");
    ESP_LOGI(TAG, "head pads: %lu %lu", (unsigned long)s_pad_base[0], (unsigned long)s_pad_base[1]);
    return ESP_OK;
}

/*
 * A pad counts as touched once its reading is CONFIG_MUSE_VOCAT_HEAD_THRESHOLD
 * tenths of a percent above its untouched level, and lets go below half that.
 * Untouched, the level follows a falling reading at once (a hand on the head
 * at boot, then lifted) and a rising one slowly (warmth, humidity), so a hand
 * isn't learned as the new normal before it's noticed.
 */
static bool head_pressed(void)
{
    static bool pressed;
    static int changing, held;
    if (!s_touch) {
        return false;
    }
    bool raw = false;
    for (int i = 0; i < s_pads; i++) {
        uint32_t v = pad_read(i), base = s_pad_base[i];
        if (!v || !base) {
            s_pad_base[i] = v;
            continue;
        }
        uint32_t on = base * CONFIG_MUSE_VOCAT_HEAD_THRESHOLD / 1000;
        if (v > base + (pressed ? on / 2 : on)) {
            raw = true;
        } else if (!pressed) {
            s_pad_base[i] = v < base ? v : base + (v - base) / 32;
        }
    }
    if (raw != pressed) {
        if (++changing < HEAD_DEBOUNCE) {
            return pressed;
        }
        pressed = raw;
        held = 0;
    }
    changing = 0;
    if (pressed && ++held >= HEAD_STUCK_POLLS) {
        ESP_LOGW(TAG, "head held a minute: taking it as the untouched level");
        for (int i = 0; i < s_pads; i++) {
            s_pad_base[i] = pad_read(i);
        }
        pressed = false;
    }
    return pressed;
}

static void head_stop(void)
{
    if (s_touch) {
        touch_sensor_stop_continuous_scanning(s_touch);
        touch_sensor_disable(s_touch);
    }
}
#else
static bool head_pressed(void)
{
    return false;
}

static void head_stop(void)
{
}
#endif

/* ---------- Board ---------- */

static esp_err_t init(void)
{
    /*
     * power_off() held these through deep sleep. Set the levels they keep
     * first, so letting go doesn't drop them: the LED off, the backlight dark
     * until the panel has a picture, the panel powered, and, if the revision
     * is known, the amp off and v1.2's codec rail on.
     */
    drive(LED_GPIO, 1);
    drive(LCD_BL, 0);
    drive(LCD_POWER, 0);
    const vocat_rev_t *known = known_rev();
    if (known) {
        drive(known->amp, 0);
    }
    if (known == &REV_1_2) {
        drive(CODEC_POWER, 1);
    }
    gpio_deep_sleep_hold_dis();
    const gpio_num_t held[] = { LCD_BL, LED_GPIO, LCD_POWER, CODEC_POWER, REV_1_0.amp, REV_1_2.amp };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        gpio_hold_dis(held[i]);
    }
    rtc_gpio_deinit(BOOT_GPIO);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    s_rev = detect_rev();
    ESP_LOGI(TAG, "ESP-VoCat %s", s_rev->name);
    drive(s_rev->amp, 0);   /* quiet until the codec opens */

    ESP_RETURN_ON_ERROR(add_device(TP_ADDR, 100000, &s_tp), TAG, "touch");
    ESP_RETURN_ON_ERROR(add_device(GAUGE_ADDR, 100000, &s_gauge), TAG, "fuel gauge");
    /* The gauge runs off the battery: without one it doesn't answer. */
    s_gauge_ok = i2c_master_probe(s_i2c, GAUGE_ADDR, 100) == ESP_OK;
    if (!s_gauge_ok) {
        ESP_LOGW(TAG, "fuel gauge not answering: no battery status");
    }
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    /* BOOT woke the board from power-off and may still be down; don't count it. */
    s_boot.pressed = gpio_get_level(BOOT_GPIO) == 0;
    s_talk_down = s_boot.pressed;
#if CONFIG_MUSE_VOCAT_HEAD_TALK
    if (head_init() != ESP_OK) {
        ESP_LOGW(TAG, "head touch unavailable");
        s_touch = NULL;
    }
#endif
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t chan = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&chan);
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, 1023 * pct / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static void IRAM_ATTR on_tp_int(void *arg)
{
    (void)arg;
    s_tp_irq = true;
}

/*
 * LVGL polls this from its own task. The CST816S naps when nobody touches it
 * and doesn't answer I2C then, so it's read only after its INT line pulses (a
 * touch, or a move) and while a finger is down, until it reports none.
 */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_tp_irq && !s_tp_down) {
        return;
    }
    s_tp_irq = false;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0) {
        s_tp_down = false;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    s_tp_down = true;
}

/* As on the Waveshare 1.75C, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    if (backlight_init() != ESP_OK) {
        return NULL;
    }
    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    /* 40 MHz, as xiaozhi runs it on both revisions; the BSP uses 80. */
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = s_vocat_lcd_init,
        .init_cmds_size = sizeof(s_vocat_lcd_init) / sizeof(s_vocat_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = s_rev->lcd_rst,
        .flags.reset_active_high = s_rev->lcd_rst_high,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    const gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(TP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    /* muse_gpio_button_init() installed the GPIO ISR service. */
    if (gpio_config(&int_cfg) != ESP_OK || gpio_isr_handler_add(TP_INT, on_tp_int, NULL) != ESP_OK) {
        return NULL;
    }
    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, tp_read);
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_sleep(void *sleep)
{
    /* SLPIN/SLPOUT over the QSPI command path, as the ST77916 driver sends commands. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* One duplex I2S bus: the ES8311 plays, the ES7210 records both mics. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = s_rev->i2s_din,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    /* The BSP's settings, with MCLK in use as xiaozhi has it: the amp runs off 5 V. */
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = s_rev->amp,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    /* MIC1 and MIC2 are the two mics, one per I2S slot; MIC3 is xiaozhi's echo reference. */
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = s_spk = esp_codec_dev_new(&out_cfg);
    *mic = s_mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

/* BOOT and the head pads are one talk button. */
static unsigned poll_buttons(void)
{
    muse_gpio_button_poll(&s_boot);
    bool down = s_boot.pressed || head_pressed();
    if (down == s_talk_down) {
        return 0;
    }
    s_talk_down = down;
    return down ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

static int gauge_word(uint8_t reg, esp_err_t *err)
{
    uint8_t b[2] = { 0 };
    esp_err_t e = reg_read(s_gauge, reg, b, sizeof(b));
    if (e != ESP_OK) {
        *err = e;
    }
    return (int16_t)(b[0] | b[1] << 8);
}

/*
 * The BQ27220 measures the battery; nothing reports USB itself. Charging
 * (not discharging, or charge current flowing) is taken as USB power, which
 * is what Muse uses it for: whether to save the battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    if (!s_gauge_ok) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_OK;
    int soc = gauge_word(GAUGE_SOC, &err);
    int mv = gauge_word(GAUGE_VOLTAGE, &err);
    int status = gauge_word(GAUGE_STATUS, &err);
    int ma = gauge_word(GAUGE_CURRENT, &err);
    if (err != ESP_OK) {
        return err;
    }
    out->battery_pct = soc < 0 ? 0 : soc > 100 ? 100 : soc;
    out->battery_mv = mv > 0 ? mv : 0;
    out->charging = !(status & GAUGE_DSG) || ma > CHARGING_MA;
    out->usb = out->charging;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x10 << 8), NULL, 0);   /* SLPIN */
}

static void hold(gpio_num_t gpio, int level)
{
    drive(gpio, level);
    gpio_hold_en(gpio);
}

/*
 * POWER is a switch the ESP32 can't reach, so "off" here is deep sleep with
 * the panel asleep and dark, the amp off and the codecs closed, until BOOT is
 * pressed. A press of POWER cuts the rest.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    if (s_panel) {
        muse_lcd_bands_run(panel_off, NULL);
    }
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    head_stop();
    ledc_stop(LEDC_LOW_SPEED_MODE, BL_CHANNEL, 0);
    /* Floating, these would light the backlight or the LED, or wake the amp. */
    hold(LCD_BL, 0);
    hold(LED_GPIO, 1);
    hold(s_rev->amp, 0);
    hold(LCD_POWER, 0);
    if (s_rev == &REV_1_2) {
        hold(CODEC_POWER, 1);   /* unpowered codecs would load the I2C lines */
    }
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    rtc_gpio_pullup_en(BOOT_GPIO);
    rtc_gpio_pulldown_dis(BOOT_GPIO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(BOOT_GPIO), ESP_EXT1_WAKEUP_ANY_LOW), TAG,
                        "boot wake");
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Espressif ESP-VoCat",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    /* Talk is BOOT, underneath, or the head above the screen. The mic sits at
     * the upper right, 40 degrees above 3 o'clock: higher, it runs into the
     * state line, which a 360 px ring brings close to the edge. */
    .talk_hint = { LV_ALIGN_CENTER, 118, -96 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = -1,             /* both mics, mixed */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
