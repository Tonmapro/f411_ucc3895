/* USER CODE BEGIN Header */
#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

#include "main.h"

// Цвета для прорисовки
#define BLACK   0
#define WHITE   1

// Прототипы базовых функций управления низкого уровня
void SSD1306_Init(void);
void SSD1306_Clear(void);
uint8_t SSD1306_Is_Busy(void);
void SSD1306_Render_DMA(void);

// Прототипы графических примитивов
void SSD1306_DrawPixel(int x, int y, uint8_t color);
void SSD1306_DrawLine(int x1, int y1, int x2, int y2, uint8_t color);

// Прототипы стандартного шрифта (8x16) для нижней зоны (Вольты, Режимы)
void SSD1306_DrawChar(int x, int y, char c, uint8_t color);
void SSD1306_DrawString(int x, int y, char* str, uint8_t color);

// Прототипы крупного шрифта (16x32) для верхней зоны (Ток)
void SSD1306_DrawBigChar(int x, int y, char c);
void SSD1306_DrawBigString(int x, int y, char* str);

// Главный диспетчер интерфейса (Автомат отрисовки)
void UI_Process_And_Draw(void);

#endif /* INC_DISPLAY_H_ */
/* USER CODE END Header */