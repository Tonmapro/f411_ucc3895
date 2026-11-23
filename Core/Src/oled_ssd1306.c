#include "oled_ssd1306.h"
#include "main.h"
#include <string.h>

// Выделяем память под ДВА буфера кадра
static uint8_t oled_buffer_A[SSD1306_BUFFER_SIZE];
static uint8_t oled_buffer_B[SSD1306_BUFFER_SIZE];

// Указатели на текущие буферы
static uint8_t *ptr_front_buffer = oled_buffer_A; // Тот, который передается по DMA
static uint8_t *ptr_back_buffer  = oled_buffer_B; // Тот, в котором процессор РИСУЕТ

static volatile OledStatus_t oled_state = OLED_READY;
static volatile uint8_t back_buffer_ready = 0; // Флаг: готов ли новый кадр для отправки

// Аппаратное управление выводами SSD1306: PB2 = DC (0=команда, 1=данные), PB10 = RESET
#define OLED_DC_CMD()   LL_GPIO_ResetOutputPin(OLED_DC_GPIO_Port, OLED_DC_Pin)
#define OLED_DC_DATA()  LL_GPIO_SetOutputPin(OLED_DC_GPIO_Port, OLED_DC_Pin)
#define OLED_RES_LOW()  LL_GPIO_ResetOutputPin(OLED_RES_GPIO_Port, OLED_RES_Pin)
#define OLED_RES_HIGH() LL_GPIO_SetOutputPin(OLED_RES_GPIO_Port, OLED_RES_Pin)

static void OLED_Start_DMA_Transfer(void);

void OLED_Clear(OledColor_t color) {
    uint8_t fill_byte = (color == OLED_COLOR_WHITE) ? 0xFFU : 0x00U;
    
    // Очищаем ТОЛЬКО задний буфер, где сейчас идет отрисовка!
    memset(ptr_back_buffer, fill_byte, SSD1306_BUFFER_SIZE);
}

void OLED_DrawPixel(uint8_t x, uint8_t y, OledColor_t color) {
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) return;

    uint16_t index = x + (y / 8U) * SSD1306_WIDTH;
    uint8_t bit_mask = (1U << (y % 8U));

    // Модифицируем ТОЛЬКО задний буфер
    if (color == OLED_COLOR_WHITE) {
        ptr_back_buffer[index] |= bit_mask;
    } else {
        ptr_back_buffer[index] &= ~bit_mask;
    }
}

// Эту функцию процессор вызывает, когда полностью закончил рисовать кадр в памяти
void OLED_Swap_Buffers(void) {
    // Если DMA занят отправкой старого кадра, мы просто взводим флаг готовности.
    // Переключение произойдет автоматически в прерывании DMA, как только оно завершится.
    if (oled_state == OLED_BUSY_DMA) {
        back_buffer_ready = 1; 
        return;
    }

    // Если DMA свободен, меняем буферы местами прямо сейчас и запускаем передачу
    uint8_t *temp = ptr_front_buffer;
    ptr_front_buffer = ptr_back_buffer;
    ptr_back_buffer = temp;

    back_buffer_ready = 0;
    OLED_Start_DMA_Transfer();
}

/* РЕЖИМ ОТПРАВКИ: 0 = polling (надёжно, для отладки), 1 = DMA (быстро, для продакшена) */
#define OLED_USE_DMA 0

// Внутренняя функция железного запуска DMA (вызывается из чистого LL)
// Внутренняя функция запуска — теперь строго на STREAM_2
static void OLED_Start_DMA_Transfer(void) {
    oled_state = OLED_BUSY_DMA;
    
    // Передача префикса команд
    OLED_DC_CMD();
    uint8_t setup_cmds[] = { 0x21, 0x00, 127, 0x22, 0x00, 7 };
    for (uint8_t i = 0; i < sizeof(setup_cmds); i++) {
        while (!LL_SPI_IsActiveFlag_TXE(SPI1));
        LL_SPI_TransmitData8(SPI1, setup_cmds[i]);
        while (LL_SPI_IsActiveFlag_BSY(SPI1)) { }
    }
    
    OLED_DC_DATA(); // Режим данных
    
#if OLED_USE_DMA
    /* Конфигурация DMA2 Stream 2 */
    LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
    while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_2)) { }

    LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_2, (uint32_t)ptr_front_buffer);
    LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_2, LL_SPI_DMA_GetRegAddr(SPI1));
    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, SSD1306_BUFFER_SIZE);
    
    LL_DMA_ClearFlag_TC2(DMA2);
    LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_2);
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
    LL_SPI_EnableDMAReq_TX(SPI1);
#else
    /* POLLING: отправка всего кадра через SPI1 без DMA */
    for (uint16_t i = 0; i < SSD1306_BUFFER_SIZE; i++) {
        while (!LL_SPI_IsActiveFlag_TXE(SPI1)) { }
        LL_SPI_TransmitData8(SPI1, ptr_front_buffer[i]);
        while (LL_SPI_IsActiveFlag_BSY(SPI1)) { }
    }
    oled_state = OLED_READY;
    
    /* Обработка отложенного кадра */
    if (back_buffer_ready) {
        uint8_t *tmp = ptr_front_buffer;
        ptr_front_buffer = ptr_back_buffer;
        ptr_back_buffer = tmp;
        back_buffer_ready = 0;
        OLED_Start_DMA_Transfer(); // рекурсивно отправит следующий кадр
    }
#endif
}

// Прерывание DMA2_Stream2 — теперь строго проверяет TC2
void OLED_DMA_IRQHandler_Callback(void) {
    if (LL_DMA_IsActiveFlag_TC2(DMA2)) { // Проверка флага TC стрима 2
        LL_DMA_ClearFlag_TC2(DMA2);      // Очистка флага TC стрима 2 за 1 такт
        
        while (LL_SPI_IsActiveFlag_BSY(SPI1)); // Ожидание выдачи последнего байта в шину
        LL_SPI_DisableDMAReq_TX(SPI1);
        /* Очистка флагов приёма: при TX-only полнодуплексе принятые байты копятся
           в RX-буфере (RXNE) и могут поднять OVR. Это не блокирует TX, но чистим
           для корректного состояния SPI перед следующим кадром. */
        if (LL_SPI_IsActiveFlag_RXNE(SPI1)) { (void)LL_SPI_ReceiveData8(SPI1); }
        LL_SPI_ClearFlag_OVR(SPI1);

        oled_state = OLED_READY;
        
        // Авто-своп буферов при готовности нового кадра
        if (back_buffer_ready) {
            uint8_t *temp = ptr_front_buffer;
            ptr_front_buffer = ptr_back_buffer;
            ptr_back_buffer = temp;
            
            back_buffer_ready = 0;
            OLED_Start_DMA_Transfer();
        }
    }
}

OledStatus_t OLED_GetStatus(void) {
    return oled_state;
}

uint8_t* OLED_Get_BackBuffer_Ptr(void) {
    return ptr_back_buffer;
}

/* ТЕСТОВЫЙ РЕЖИМ: вывод всего экрана чистым polling без DMA.
   Если дисплей показывает тест — SPI/дисплей исправны, проблема в DMA. */
void OLED_Test_Polling(void) {
    static uint8_t test_buffer[SSD1306_BUFFER_SIZE];
    memset(test_buffer, 0x00, SSD1306_BUFFER_SIZE);

    /* Рисуем рамку по краям экрана (видна если всё работает) */
    for (uint16_t x = 0; x < SSD1306_WIDTH; x++) {
        test_buffer[x] |= 0x01;                              // верхняя строка
        test_buffer[x + (SSD1306_HEIGHT/8-1)*SSD1306_WIDTH] |= 0x80; // нижняя строка
    }
    for (uint16_t y = 0; y < SSD1306_HEIGHT; y++) {
        uint16_t page = y / 8;
        uint8_t bit = 1U << (y % 8);
        test_buffer[0 + page * SSD1306_WIDTH] |= bit;          // левый столбец
        test_buffer[SSD1306_WIDTH-1 + page * SSD1306_WIDTH] |= bit; // правый
    }

    /* Отправка команд + данные чистым polling */
    OLED_DC_CMD();
    uint8_t setup_cmds[] = { 0x21, 0x00, 127, 0x22, 0x00, 7 };
    for (uint8_t i = 0; i < sizeof(setup_cmds); i++) {
        while (!LL_SPI_IsActiveFlag_TXE(SPI1)) { }
        LL_SPI_TransmitData8(SPI1, setup_cmds[i]);
        while (LL_SPI_IsActiveFlag_BSY(SPI1)) { }
    }
    OLED_DC_DATA();
    for (uint16_t i = 0; i < SSD1306_BUFFER_SIZE; i++) {
        while (!LL_SPI_IsActiveFlag_TXE(SPI1)) { }
        LL_SPI_TransmitData8(SPI1, test_buffer[i]);
        while (LL_SPI_IsActiveFlag_BSY(SPI1)) { }
    }
}

static void OLED_Write_Command(uint8_t cmd) {
    OLED_DC_CMD();
    while (!LL_SPI_IsActiveFlag_TXE(SPI1)) { }
    LL_SPI_TransmitData8(SPI1, cmd);
    while (LL_SPI_IsActiveFlag_BSY(SPI1)) { }
}

void OLED_Init(void) {
    /* Защитное включение SPI1: если по какой-то причине MX_SPI1_Init() не включил
       SPE (SPI_CR1), дисплей физически не увидит тактов/данных. LL_SPI_Enable
       идемпотентен и гарантирует, что SSD1306 получит команду. */
    if (!LL_SPI_IsEnabled(SPI1)) {
        LL_SPI_Enable(SPI1);
    }
    OLED_DC_CMD();   /* на всякий случай: DC в командный режим до начала посылки */
    OLED_RES_HIGH(); /* RES не должен висеть в 0, иначе контроллер в сбросе */

    /* Аппаратный сброс контроллера (PB10). LL_mDelay опирается на LL_IncTick()
       из SysTick_Handler — см. stm32f4xx_it.c */
    OLED_RES_LOW();
    LL_mDelay(10);
    OLED_RES_HIGH();
    LL_mDelay(50);

    /* Стандартная последовательность инициализации SSD1306 128x64 */
    static const uint8_t init_seq[] = {
        0xAE,       // Display OFF
        0xD5, 0x80, // Тактирование (OSC, делитель 0)
        0xA8, 0x3F, // Мультиплексирование 1/64
        0xD3, 0x00, // Смещение дисплея 0
        0x40,       // Начальная строка 0
        0x8D, 0x14, // Внутренний charge pump ВКЛ
        0x20, 0x00, // Горизонтальный режим адресации
        0xA1,       // Segment remap: SEG0 -> столбец 127
        0xC8,       // COM сканирование снизу вверх
        0xDA, 0x12, // Распиновка COM (альтернативная конфигурация)
        0x81, 0xCF, // Контраст
        0xD9, 0xF1, // Pre-charge period
        0xDB, 0x40, // VCOMH ~0.77xVCC
        0xA4,       // Вывод содержимого RAM (не "всё включено")
        0xA6,       // Нормальная полярность (без инверсии)
        0xAF        // Display ON
    };
    for (uint32_t i = 0; i < sizeof(init_seq); i++) {
        OLED_Write_Command(init_seq[i]);
    }

    /* Оба буфера чистые, первый пустой кадр сразу уходит по DMA */
    memset(ptr_back_buffer, 0x00U, SSD1306_BUFFER_SIZE);
    memset(ptr_front_buffer, 0x00U, SSD1306_BUFFER_SIZE);
    OLED_Swap_Buffers();
}
