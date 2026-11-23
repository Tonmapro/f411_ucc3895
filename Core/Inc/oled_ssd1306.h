#ifndef DRIVERS_HARDWARE_INC_OLED_SSD1306_H_
#define DRIVERS_HARDWARE_INC_OLED_SSD1306_H_

#include <stdint.h>

#define SSD1306_WIDTH        128U
#define SSD1306_HEIGHT       64U
#define SSD1306_BUFFER_SIZE  ((SSD1306_WIDTH * SSD1306_HEIGHT) / 8U)

typedef enum {
    OLED_COLOR_BLACK = 0x00U,
    OLED_COLOR_WHITE = 0x01U
} OledColor_t;

typedef enum {
    OLED_READY = 0,
    OLED_BUSY_DMA
} OledStatus_t;

void         OLED_Init(void);
void         OLED_Test_Polling(void);
void         OLED_Clear(OledColor_t color);
void         OLED_DrawPixel(uint8_t x, uint8_t y, OledColor_t color);
void         OLED_Swap_Buffers(void); // <--- Новая функция вместо старой асинхронной отправки
void         OLED_DMA_IRQHandler_Callback(void); // <--- Новый обработчик прерывания
OledStatus_t OLED_GetStatus(void);
uint8_t*     OLED_Get_BackBuffer_Ptr(void); // Указатель на буфер, в котором идёт рендеринг

#endif /* DRIVERS_HARDWARE_INC_OLED_SSD1306_H_ */
