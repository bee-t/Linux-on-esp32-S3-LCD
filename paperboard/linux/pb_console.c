// ESP-IDF side of the Paperboard text console for GC9107 LCD display.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "terminal.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// Define ESP_PLATFORM for font8x8_basic.h
#ifndef ESP_PLATFORM
#define ESP_PLATFORM
#endif
#ifndef PB_FONT_STORAGE
#define PB_FONT_STORAGE
#endif
#include "font8x8_basic.h"

static volatile const bool headless_build = false;
static PbTerminal current;
static QueueHandle_t queue;

#define PIN_NUM_MISO -1
#define PIN_NUM_MOSI 39
#define PIN_NUM_CLK  40
#define PIN_NUM_CS   41
#define PIN_NUM_DC   42
#define PIN_NUM_RST  38
#define PIN_NUM_BCKL 14

static spi_device_handle_t spi;
static uint16_t* framebuffer;

struct chunk { unsigned len; uint8_t bytes[128]; };

static void lcd_cmd(spi_device_handle_t spi_handle, const uint8_t cmd) {
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void*)0; // DC=0 for command
    ret = spi_device_polling_transmit(spi_handle, &t);
    configASSERT(ret == ESP_OK);
}

static void lcd_data(spi_device_handle_t spi_handle, const uint8_t *data, int len) {
    esp_err_t ret;
    spi_transaction_t t;
    if (len == 0) return;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void*)1; // DC=1 for data
    ret = spi_device_polling_transmit(spi_handle, &t);
    configASSERT(ret == ESP_OK);
}

static void lcd_spi_pre_transfer_callback(spi_transaction_t *t) {
    int dc = (int)t->user;
    gpio_set_level(PIN_NUM_DC, dc);
}

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;
} lcd_init_cmd_t;

static const lcd_init_cmd_t gc9107_init_cmds[] = {
    {0xFE, {0}, 0},
    {0xEF, {0}, 0},
    {0xEB, {0x14}, 1},
    {0xFE, {0}, 0},
    {0xEF, {0}, 0},
    {0xEB, {0x14}, 1},
    {0x84, {0x40}, 1},
    {0x85, {0xFF}, 1},
    {0x86, {0xFF}, 1},
    {0x87, {0xFF}, 1},
    {0x88, {0x0A}, 1},
    {0x89, {0x21}, 1},
    {0x8A, {0x00}, 1},
    {0x8B, {0x80}, 1},
    {0x8C, {0x01}, 1},
    {0x8D, {0x01}, 1},
    {0x8E, {0xFF}, 1},
    {0x8F, {0xFF}, 1},
    {0xB6, {0x00, 0x20}, 2},
    {0x3A, {0x05}, 1}, // COLMOD: 16bpp
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4},
    {0xBD, {0x06}, 1},
    {0xBC, {0x00}, 1},
    {0xFF, {0x60, 0x01, 0x04}, 3},
    {0xC3, {0x13}, 1},
    {0xC4, {0x13}, 1},
    {0xC9, {0x22}, 1},
    {0xBE, {0x11}, 1},
    {0xE1, {0x10, 0x0E}, 2},
    {0xDF, {0x21, 0x0c, 0x02}, 3},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
    {0xED, {0x1B, 0x0B}, 2},
    {0xAE, {0x77}, 1},
    {0xCD, {0x63}, 1},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9},
    {0xE8, {0x34}, 1},
    {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12},
    {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7},
    {0x98, {0x3e, 0x07}, 2},
    {0x35, {0}, 0},
    {0x21, {0}, 0}, // INVON
    {0x11, {0}, 0x80}, // SLPOUT + 120ms delay
    {0x29, {0}, 0x80}, // DISPON + delay
    {0, {0}, 0xff}
};

static void lcd_set_window(spi_device_handle_t spi_handle, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += 2; x1 += 2;
    y0 += 1; y1 += 1;
    lcd_cmd(spi_handle, 0x2A);
    uint8_t d_x[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    lcd_data(spi_handle, d_x, 4);
    lcd_cmd(spi_handle, 0x2B);
    uint8_t d_y[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_data(spi_handle, d_y, 4);
    lcd_cmd(spi_handle, 0x2C);
}

static void console_task(void* arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    bool pending = true;
    for(;;) {
        struct chunk c;
        if(xQueueReceive(queue, &c, pdMS_TO_TICKS(20)) == pdTRUE) {
            pb_feed(&current, c.bytes, c.len);
            pending = true;
        }
        if(!pending || xTaskGetTickCount() - last < pdMS_TO_TICKS(100)) continue;
        
        for (int row=0; row<PB_ROWS; row++) {
            for (int col=0; col<PB_COLS; col++) {
                PbCell cell = current.cells[row][col];
                unsigned char const* glyph = font8x8_basic[cell.ch < 128 ? cell.ch : '?'];
                uint16_t bg = cell.inverse ? 0xFFFF : 0x0000;
                uint16_t fg = cell.inverse ? 0x0000 : 0xFFFF;
                if (current.cursor_visible && current.x == col && current.y == row) {
                    fg = 0xF800; // Red cursor
                    bg = 0xFFFF;
                }
                
                for (int y=0; y<8; y++) {
                    unsigned char line = glyph[y];
                    for (int x=0; x<8; x++) {
                        uint16_t color = (line & (1 << x)) ? fg : bg;
                        int px = col * 8 + x;
                        int py = row * 8 + y;
                        framebuffer[py * 128 + px] = (color >> 8) | (color << 8); // GC9107 expects big-endian pixels
                    }
                }
            }
        }
        
        lcd_set_window(spi, 0, 0, 127, 127);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = 128 * 128 * 16;
        t.tx_buffer = framebuffer;
        t.user = (void*)1;
        spi_device_polling_transmit(spi, &t);
        
        last = xTaskGetTickCount();
        pending = false;
    }
}

void pb_console_write(const uint8_t* data, size_t len) {
    if(!queue) return;
    while(len) {
        struct chunk c = {.len = len > 128 ? 128 : len};
        memcpy(c.bytes, data, c.len);
        xQueueSend(queue, &c, portMAX_DELAY);
        data += c.len; len -= c.len;
    }
}

void pb_console_init(void) {
    if (headless_build) {
        printf("paperboard headless v1: display disabled; serial console only\n");
        return;
    }
    configASSERT(xPortGetCoreID() == 0);
    
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_BCKL, GPIO_MODE_OUTPUT);

    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    gpio_set_level(PIN_NUM_BCKL, 1); 

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128 * 128 * 2 + 8
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000, 
        .mode = 0,                               
        .spics_io_num = PIN_NUM_CS,               
        .queue_size = 7,                          
        .pre_cb = lcd_spi_pre_transfer_callback,  
    };
    
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    
    int cmd = 0;
    while (gc9107_init_cmds[cmd].databytes != 0xff) {
        lcd_cmd(spi, gc9107_init_cmds[cmd].cmd);
        if (gc9107_init_cmds[cmd].databytes & 0x1F) {
            lcd_data(spi, gc9107_init_cmds[cmd].data, gc9107_init_cmds[cmd].databytes & 0x1F);
        }
        if (gc9107_init_cmds[cmd].databytes & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        cmd++;
    }

    lcd_cmd(spi, 0x36); // MADCTL
    uint8_t madctl = 0xC8;
    lcd_data(spi, &madctl, 1);

    framebuffer = heap_caps_malloc(128 * 128 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    configASSERT(framebuffer);

    pb_init(&current);
    const char* banner = "GC9107 128x128\r\nLinux on ESP32-S3\r\n\r\n";
    pb_feed(&current, (const uint8_t*)banner, strlen(banner));
    
    queue = xQueueCreate(8, sizeof(struct chunk));
    configASSERT(queue);
    
    configASSERT(xTaskCreatePinnedToCore(console_task, "pb_console", 4096, NULL, 2, NULL, 0) == pdPASS);
}
