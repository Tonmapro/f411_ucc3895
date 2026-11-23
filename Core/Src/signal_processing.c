#include "signal_processing.h"
#include "config.h" 
#include <string.h>
#include <stddef.h>
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_adc.h"
#include "stm32f4xx_ll_dma.h"

/* Проверки геометрии буферов на этапе компиляции */
_Static_assert(ADC_BUFFER_SIZE == (ADC_CHANNELS * ADC_OVERSAMPLING), 
    "Критическая ошибка: Неверное соотношение каналов и оверсэмплинга в config.h!");

_Static_assert(ADC_OVERSAMPLING <= 256, 
    "Критическая ошибка: Слишком большое окно оверсэмплинга, уменьшите размер!");

/* Выравнивание буфера DMA на границу 4 байт для корректной работы шины AHB */
__attribute__((aligned(4))) static uint16_t adc_raw_buffer[PING_PONG_BUFF_SIZE];

/* Внутренние и публичные буферы отфильтрованных кодов (Строго ADC_CHANNELS = 6) */
static float filtered_values[ADC_CHANNELS] = {0.0f};
static float public_filtered_values[ADC_CHANNELS] = {0.0f};

/* Коэффициенты пересчета суммы оверсэмплинга обратно в чистый 12-битный код АЦП (0..4095).
   Активны все 6 физических каналов (ADC_CHANNELS=6). Физические масштабы (Вольты/Амперы)
   заданы в config.h (CURRENT_SCALE, VOLTAGE_SCALE...) и применяются на верхнем уровне. */
static const float adc_gains[ADC_CHANNELS] = {
    [ADC_CURRENT_IDX]  = 1.0f / (float)ADC_OVERSAMPLING,
    [ADC_VOLTAGE_IDX]  = 1.0f / (float)ADC_OVERSAMPLING,
    [ADC_V15V_IDX]     = 1.0f / (float)ADC_OVERSAMPLING,
    [ADC_TEMP_IDX]     = 1.0f / (float)ADC_OVERSAMPLING,
    [ADC_TEMP_DIO_IDX] = 1.0f / (float)ADC_OVERSAMPLING,
    [ADC_FAN_CURR_IDX] = 1.0f / (float)ADC_OVERSAMPLING
};

/* Коэффициенты сглаживания фильтра Бесселя 1-го порядка (EMA) */
static const float filter_alphas[ADC_CHANNELS] = {
    [ADC_CURRENT_IDX]  = 0.30f,  // Высокая скорость для ПИ-регулятора
    [ADC_VOLTAGE_IDX]  = 0.20f,
    [ADC_V15V_IDX]     = 0.05f,  // Медленный фильтр для питающей сети 15В
    [ADC_TEMP_IDX]     = 0.01f,  // Тяжелый фильтр для инерционного термодатчика
    [ADC_TEMP_DIO_IDX] = 0.01f,  // Тяжелый фильтр для инерционного термодатчика
    [ADC_FAN_CURR_IDX] = 0.05f
};

static volatile uint8_t  dma_process_ready = 0U;
static volatile uint32_t dma_active_start_idx = 0U;

void Signal_Processing_Init(void)
{
    memset((void*)adc_raw_buffer, 0, sizeof(adc_raw_buffer));
    for (uint32_t i = 0; i < ADC_CHANNELS; i++)
    {
        filtered_values[i] = 0.0f;
        public_filtered_values[i] = 0.0f;
    }

    /* 
     * ЗАПУСК АЦП И DMA НА ЧИСТОМ LL (Взамен HAL_ADC_Start_DMA):
     * Настройка адресов в регистры DMA2 (Пример для ADC1, находящегося на Stream 0, Channel 0)
     */
    LL_DMA_ConfigAddresses(DMA2, LL_DMA_STREAM_0, 
                           LL_ADC_DMA_GetRegAddr(ADC1, LL_ADC_DMA_REG_REGULAR_DATA), // Откуда
                           (uint32_t)adc_raw_buffer,                                 // Куда
                           LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    
    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_0, PING_PONG_BUFF_SIZE);
    
    /* Включаем прерывания половины передачи (HT) и полной передачи (TC) на уровне LL DMA */
    LL_DMA_EnableIT_HT(DMA2, LL_DMA_STREAM_0);
    LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_0);
    
    /* Активируем потоки */
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_0);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);

    /*
     * ЗАПУСК АЦП ПО RM0383 (гл. 13): у STM32F4 ADC нет бита ADCAL (калибровка
     * заводская) и флага ADRDY (это фичи F3/F7), поэтому включение простое.
     */
    LL_ADC_Enable(ADC1);

    /* ИСПРАВЛЕНО (баг «220V-!» на макете, найден 2026-09):
       LL_ADC_REG_StartConversionExtTrig(ADC1, LL_ADC_REG_TRIG_SOFTWARE) —
       это SET_BIT(CR2, 0x0), т.е. no-op: бит SWSTART не выставлялся, АЦП ни
       разу не конвертировал, все коды оставались 0, и FSM первым же тиком
       защёлкивалась в FAULT_CODE_LOW_15V («220V-!») независимо от уровня PA2.
       Процедура SW-старта по RM0383 §13.7: EXTEN = 00 (иначе SWSTART
       игнорируется — сгенерированный CubeMX код adc.c оставил EXTEN=RISING),
       затем SWSTART = 1. */
    LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE); /* EXTSEL = SW start */
    CLEAR_BIT(ADC1->CR2, ADC_CR2_EXTEN);                         /* EXTEN = 00        */
    LL_ADC_REG_StartConversionSWStart(ADC1);                     /* SWSTART = 1       */

    /* ОТКАТ (V13, подтверждено A/B-экспериментом на макете через BMP, 2026-09):
       прежняя правка переводила PA3/PA5/PB0 в цифровой INPUT с подтяжкой ~40 кОм
       ради «детекции обрыва датчика». Это ЛОМАЛО измерение: при неизменном
       делителе 10к/10к на PA3 ранг-4 DMA давал ≈390-440 кодов, а после перевода
       пина обратно в ANALOG — ≈2031-2033 (ожидаемые ≈2048). Подтяжка/цифровой
       вход вносят смещение в измерительный узел. По RM0383 §8.3.12 входы АЦП
       должны работать в ANALOG: высокий импеданс, подтяжки отключены.
       Детекция обрыва датчика на реальной плате обеспечивается ВНЕШНЕЙ подтяжкой
       к +3.3 В в топологии делителя (обрыв → узел → 3.3 В → код ≈4095 →
       «TSENSOR»), а не режимом GPIO. На макете без датчиков обязательны
       заглушки-делители (Docs/bringup_guide.md §1.2). */
}

void Signal_Processing_Update(void)
{
    if (!dma_process_ready) return;
    
    uint32_t start_idx = dma_active_start_idx;
    dma_process_ready = 0U; 

    uint32_t accumulators[ADC_CHANNELS] = {0};
    uint16_t *p_buf = &adc_raw_buffer[start_idx];

    /* 
     * ДЕИНТЕРЛИВИНГ СТРОГО ПО ГЕОМЕТРИИ ПОСЛЕДОВАТЕЛЬНОСТИ АЦП:
     * 6 каналов (Rank1..Rank6 = IN0,IN1,IN2,IN3,IN5,IN8), шаг указателя = 6 элементов за проход.
     * Буфер половины = ADC_BUFFER_SIZE = 6*16 = 96 элементов, читается ровно без выхода за границу.
     */
    for (uint32_t i = 0; i < ADC_OVERSAMPLING; i++)
    {
        accumulators[ADC_CURRENT_IDX]  += (*p_buf++) & 0x0FFFU;
        accumulators[ADC_VOLTAGE_IDX]  += (*p_buf++) & 0x0FFFU;
        accumulators[ADC_V15V_IDX]     += (*p_buf++) & 0x0FFFU;
        accumulators[ADC_TEMP_IDX]     += (*p_buf++) & 0x0FFFU;
        accumulators[ADC_TEMP_DIO_IDX] += (*p_buf++) & 0x0FFFU;
        accumulators[ADC_FAN_CURR_IDX] += (*p_buf++) & 0x0FFFU;
    }
    
    for (uint32_t ch = 0; ch < ADC_CHANNELS; ch++)
    {
        // Переводим накопленную сумму в чистый 12-битный код АЦП (0..4095)
        float instant_code = (float)accumulators[ch] * adc_gains[ch];
        
        // Экспоненциальное сглаживание фильтра Бесселя
        filtered_values[ch] += filter_alphas[ch] * (instant_code - filtered_values[ch]);
    }
}

/**
 * @brief  Низкоуровневые LL-обработчики прерываний DMA2 (Вызывать из stm32f4xx_it.c)
 * @note   Полностью заменяют тяжелые HAL-коллбэки HalfCplt и ConvCplt
 */
void Signal_Processing_DMA_HT_Callback(void) {
    dma_active_start_idx = 0U; // Первую половину буфера можно безопасно читать
    dma_process_ready = 1U;
}

void Signal_Processing_DMA_TC_Callback(void) {
    dma_active_start_idx = ADC_BUFFER_SIZE; // Вторую половину буфера можно безопасно читать
    dma_process_ready = 1U;
}

void Signal_Processing_Publish(void)
{
    /* 
     * ИСПРАВЛЕНИЕ БЕЗОПАСНОСТИ СИЛОВОЙ ЧАСТИ:
     * Удален макрос __disable_irq(). На 32-битной архитектуре ARM Cortex-M4 
     * копирование одиночной float переменной атомарно на уровне транзакции шины.
     */
    public_filtered_values[ADC_CURRENT_IDX]  = filtered_values[ADC_CURRENT_IDX];
    public_filtered_values[ADC_VOLTAGE_IDX]  = filtered_values[ADC_VOLTAGE_IDX];
    public_filtered_values[ADC_V15V_IDX]     = filtered_values[ADC_V15V_IDX];
    public_filtered_values[ADC_TEMP_IDX]     = filtered_values[ADC_TEMP_IDX];
    public_filtered_values[ADC_TEMP_DIO_IDX] = filtered_values[ADC_TEMP_DIO_IDX];
    public_filtered_values[ADC_FAN_CURR_IDX] = filtered_values[ADC_FAN_CURR_IDX];
}

float Signal_Processing_Get_Filtered_Code(uint32_t channel_idx)
{
    if (channel_idx >= ADC_CHANNELS) return 0.0f;
    return public_filtered_values[channel_idx];
}

/* Реализация интерфейсных функций, которые жестко требует прерывание TIM2 из main.c */
uint16_t Signal_Processing_Get_Voltage(void) {
    return (uint16_t)(public_filtered_values[ADC_VOLTAGE_IDX] + 0.5f);
}

uint16_t Signal_Processing_Get_Current(void) {
    return (uint16_t)(public_filtered_values[ADC_CURRENT_IDX] + 0.5f);
}
