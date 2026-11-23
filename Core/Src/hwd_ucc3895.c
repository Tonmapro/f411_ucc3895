#include "hwd_ucc3895.h"
#include "config.h"
#include "main.h" // Содержит дефайны UCC_DISABLE_Pin, UCC_FAULT_Pin и LL-драйверы

volatile uint8_t global_fault_flag = 0;

/**
 * @brief  Мгновенный аппаратный останов UCC3895.
 * @note   PA4 -> HIGH: внешний N-канальный ключ открывается и прижимает вывод
 *         SS/DISABLE UCC3895 к GND. По datasheet UCC3895 (п. 8.3.5 Soft-Start/
 *         Disable) уровень < 0.5 В на этом выводе принудительно обнуляет
 *         фазовый сдвиг и блокирует выходные драйверы. Запись в BSRR — 1 такт.
 */
void UCC3895_Disable(void)
{
    LL_GPIO_SetOutputPin(UCC_DISABLE_GPIO_Port, UCC_DISABLE_Pin);
}

/**
 * @brief  Аппаратное включение (SS/DISABLE = отпущен, soft-start от внешнего конденсатора).
 * @note   Атомарно защищено: вызов игнорируется при активном флаге аварии.
 */
void UCC3895_Enable(void)
{
    if (global_fault_flag == 0U) {
        LL_GPIO_ResetOutputPin(UCC_DISABLE_GPIO_Port, UCC_DISABLE_Pin);
    }
}

/**
 * @brief  Установка управляющего напряжения на входе EAP UCC3895 (ШИМ TIM4 + внешний ФНЧ).
 * @note   Математика (PWM1, up-counting, RM0383 гл. 19): выход активен пока CNT < CCR,
 *         поэтому CCR = 0 даёт 0 %, а CCR = ARR+1 = 3840 (> ARR) — честные 100 %:
 *             duty = V / 3.3;  CCR = duty * (ARR + 1)
 *         Разрешение 3840 ступеней (~0.86 мВ) при частоте ШИМ 25 кГц (96 МГц / 3840).
 */
void UCC3895_Set_Control_Voltage(float volts)
{
    if (volts < 0.0f) volts = 0.0f;
    if (volts > 3.3f) volts = 3.3f;

    uint32_t ccr = (uint32_t)(((volts * (float)UCC3895_PWM_PERIOD_TICKS) / 3.3f) + 0.5f);
    if (ccr > UCC3895_PWM_PERIOD_TICKS) ccr = UCC3895_PWM_PERIOD_TICKS;

    LL_TIM_OC_SetCompareCH1(TIM4, (uint32_t)ccr);
}

uint8_t UCC3895_Is_Fault_Active(void)
{
    return global_fault_flag;
}

void UCC3895_Clear_Fault(void)
{
    global_fault_flag = 0U;
    Inverter.state = SYS_STATE_STANDBY;
}

void UCC3895_Init(void)
{
    /* 1. Гарантированно удерживаем силовую часть в выключенном состоянии при старте */
    UCC3895_Disable();

    /* 2. Полная инициализация таймера виртуального ЦАП (TIM4_CH1) чистым LL */
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4);

    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    TIM_InitStruct.Prescaler = 0;
    TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
    TIM_InitStruct.Autoreload = 3839U; // UCC3895_ARR_MAX (период ШИМ 25 кГц)
    TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    LL_TIM_Init(TIM4, &TIM_InitStruct);

    // Включаем буферизацию (Preload) регистра периода ARR
    LL_TIM_EnableARRPreload(TIM4);

    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_PWM1;
    TIM_OC_InitStruct.OCState = LL_TIM_OCSTATE_ENABLE;
    TIM_OC_InitStruct.CompareValue = 0; // На старте выдаем 0 Вольт
    TIM_OC_InitStruct.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    LL_TIM_OC_Init(TIM4, LL_TIM_CHANNEL_CH1, &TIM_OC_InitStruct);
    LL_TIM_OC_EnablePreload(TIM4, LL_TIM_CHANNEL_CH1);

    /* Принудительно генерируем обновление для переноса данных из Preload в теневые регистры */
    LL_TIM_GenerateEvent_UPDATE(TIM4);

    // Запускаем таймер и активируем выход канала 1
    LL_TIM_EnableCounter(TIM4);
    LL_TIM_CC_EnableChannel(TIM4, LL_TIM_CHANNEL_CH1);

    /* 3. Конфигурация пина ШИМ PB6 = TIM4_CH1 (AF2), тактовая кнопка GPIOB уже включена в MX_GPIO_Init */
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = LL_GPIO_PIN_6;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    GPIO_InitStruct.Alternate = LL_GPIO_AF_2;
    LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* 4. Настройка аппаратного прерывания EXTI для пина UCC_FAULT (Пин PB12) */
#if defined(UCC_FAULT_Pin)

    /* Включаем тактирование SYSCFG для маршрутизации EXTI */
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

    /* Привязываем линию EXTI12 к порту GPIOB (PB12) */
    LL_SYSCFG_SetEXTISource(LL_SYSCFG_EXTI_PORTB, LL_SYSCFG_EXTI_LINE12);

    /*
     * Выделяем маску линии EXTI.
     * Для пина 12 (PB12) маской является строго макрос LL_EXTI_LINE_12 (или UCC_FAULT_Pin).
     */
    uint32_t exti_line = UCC_FAULT_Pin;

    // Запрещаем прерывание линии на время переконфигурации триггеров
    LL_EXTI_DisableIT_0_31(exti_line);

    /*
     * Триггер строго на СПАД уровня (Falling): авария прижимает пин к 0В
     * (подтяжка к питанию включена в MX_GPIO_Init).
     */
    LL_EXTI_EnableFallingTrig_0_31(exti_line);
    LL_EXTI_DisableRisingTrig_0_31(exti_line);

    /* Очищаем флаг именно ЛИНИИ прерывания перед включением */
    LL_EXTI_ClearFlag_0_31(exti_line);

    // Разрешаем прерывание линии в контроллере EXTI
    LL_EXTI_EnableIT_0_31(exti_line);

    /* 5. Настройка NVIC (Вектор EXTI15_10_IRQn для 12-й линии) */
    // Приоритет 0 — абсолютная защита силовой части инвертора
    NVIC_SetPriority(EXTI15_10_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 0, 0));
    NVIC_EnableIRQ(EXTI15_10_IRQn);
#endif
}
