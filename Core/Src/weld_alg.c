
#include "stdint.h"            // Стандартные типы данных (uint32_t)
#include "stm32f4xx_ll_cortex.h" // Содержит макрос LL_SYSTICK_IsActiveCounterFlag
#include "stm32f4xx_ll_rcc.h"    // Для работы с тактовым генератором RCC
#include "weld_alg.h"
#include "config.h"
#include "main.h"
#include "stm32f4xx_ll_tim.h"
#include "stm32f411xe.h"

// HAL-дескриптор htim4 удален. LL работает напрямую с базовым адресом TIM4.
static uint32_t state_timer_ms = 0U;

void Weld_Alg_Init(void) {
    // Безопасный старт: уставка на минимум
    Weld_Alg_Set_Output_Current(WELD_CURRENT_MIN);
    
    // Включение канала 3 ШИМ на таймере TIM4 (Аналог HAL_TIM_PWM_Start)
    LL_TIM_CC_EnableChannel(TIM4, TIM_CCER_CC3E );
    
    Inverter.state = SYS_STATE_STANDBY;
}

/**
  * @brief  Высокоскоростной автомат обработки дуги. Вызывается раз в 1 мс.
  * @param  raw_adc_v: Прямое несглаженное значение АЦП напряжения
  * @param  raw_adc_i: Прямое несглаженное значение АЦП тока
  */
void Weld_Alg_Periodic_Process(uint16_t raw_adc_v, uint16_t raw_adc_i) {
    // Сохраняем сырые данные для UI слоя (пересчет во float будет там!)
    Inverter.actual_voltage = raw_adc_v;
    Inverter.actual_current = raw_adc_i;

    uint16_t next_current_setpoint = (uint16_t)Inverter.target_current;

    // Оборонительное программирование: валидация уставки тока пользователя
    if (next_current_setpoint < WELD_CURRENT_MIN) next_current_setpoint = WELD_CURRENT_MIN;
    if (next_current_setpoint > WELD_CURRENT_MAX) next_current_setpoint = WELD_CURRENT_MAX;

    switch (Inverter.state) {
        
        case SYS_STATE_STANDBY:
            // Напряжение ХХ. Если упало ниже порога — зафиксирован поджиг дуги
            if (raw_adc_v < ADC_THRES_NO_LOAD) {
                Inverter.state = SYS_STATE_HOT_START;
                state_timer_ms = 0U;
            }
            break;

        case SYS_STATE_HOT_START:
            // Наброс тока +40% для гарантированного поджига без залипания
            next_current_setpoint = next_current_setpoint + (next_current_setpoint * 40U / 100U);
            if (next_current_setpoint > WELD_CURRENT_MAX) {
                next_current_setpoint = WELD_CURRENT_MAX;
            }
            
            state_timer_ms++;
            if (state_timer_ms >= MMA_HOT_START_TIME_MS) {
                Inverter.state = SYS_STATE_WELDING;
            }
            
            // Если при поджиге намертво засадили электрод
            if (raw_adc_v < ADC_THRES_ANTI_STICK) {
                Inverter.state = SYS_STATE_ANTI_STICK;
                state_timer_ms = 0U;
            }
            break;

        case SYS_STATE_WELDING:
            // Основной рабочий режим
            if (raw_adc_v < ADC_THRES_ARC_FORCE && raw_adc_v >= ADC_THRES_ANTI_STICK) {
                Inverter.state = SYS_STATE_ARC_FORCE;
            } else if (raw_adc_v < ADC_THRES_ANTI_STICK) {
                Inverter.state = SYS_STATE_ANTI_STICK;
                state_timer_ms = 0U;
            } else if (raw_adc_v > (ADC_THRES_NO_LOAD + 200U)) { // Дуга разорвана
                Inverter.state = SYS_STATE_STANDBY;
            }
            break;

        case SYS_STATE_ARC_FORCE:
            // Форсаж: чем ниже напряжение дуги, тем сильнее наброс тока
            if (raw_adc_v < ADC_THRES_ARC_FORCE) {
                uint32_t delta_v = ADC_THRES_ARC_FORCE - raw_adc_v;
                // Коэффициент форсирования: +1 Ампер уставки на каждые 4 единицы падения АЦП
                next_current_setpoint += (uint16_t)(delta_v / 4U);
                if (next_current_setpoint > WELD_CURRENT_MAX) {
                    next_current_setpoint = WELD_CURRENT_MAX;
                }
            }

            // Выход из форсажа
            if (raw_adc_v >= ADC_THRES_ARC_FORCE) {
                Inverter.state = SYS_STATE_WELDING;
            } else if (raw_adc_v < ADC_THRES_ANTI_STICK) {
                Inverter.state = SYS_STATE_ANTI_STICK;
                state_timer_ms = 0U;
            }
            break;

        case SYS_STATE_ANTI_STICK:
            // Защита от прокаливания электрода при КЗ
            if (raw_adc_v < ADC_THRES_ANTI_STICK) {
                state_timer_ms++;
                if (state_timer_ms >= MMA_ANTI_STICK_TIME_MS) {
                    // Электрод прилип окончательно. Сбрасываем ток до безопасного дежурного минимума.
                    next_current_setpoint = MMA_ANTI_STICK_CURR;
                }
            } else {
                // Электрод оторван, напряжение выросло — возвращаемся в режим ожидания
                Inverter.state = SYS_STATE_STANDBY;
            }
            break;

        case SYS_STATE_FAULT:
            // Аварийный режим. Ток строго в ноль, ШИМ отключен.
            next_current_setpoint = 0U;
            break;

        default:
            // Защита от неопределенного состояния памяти (Fallback)
            Inverter.state = SYS_STATE_FAULT;
            break;
    }

    // Применение уставки тока
    Weld_Alg_Set_Output_Current(next_current_setpoint);
}

void Weld_Alg_Set_Output_Current(uint16_t target_amps) {

    uint32_t ccr_value = 0U; 
    // Защита «железа»: ограничение на уровне функции записи в регистры
    if (target_amps > WELD_CURRENT_MAX && Inverter.state != SYS_STATE_HOT_START) {
        target_amps = WELD_CURRENT_MAX;
    }
    
    // Пересчет тока в уставку таймера TIM4_CH3 (ARR = 1000)
    // 180 Ампер = 900 единиц (90% заполнения для UCC3895)
    uint32_t ccr_value = ((uint32_t)target_amps * 900U) / WELD_CURRENT_MAX;
    
    // Аппаратный барьер
    if (ccr_value > 900U) ccr_value = 900U;
    if (Inverter.state == SYS_STATE_FAULT) ccr_value = 0U;

    // Прямая запись в регистр CCR3 таймера TIM4 через LL (Аналог __HAL_TIM_SET_COMPARE)
    LL_TIM_OC_SetCompareCH3(TIM4, ccr_value);
    
    Inverter.out_dac_value = (uint16_t)ccr_value;
}
