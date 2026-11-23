/* USER CODE BEGIN Header */
#include "welder.h"

// Масштабные коэффициенты под измерительные цепи схемы
#define ADC_VOLTAGE_SCALE   (3.3f / 4095.0f * 31.0f)  // Выходной делитель напряжения
#define ADC_CURRENT_SCALE   (3.3f / 4095.0f * 100.0f) // Датчик Холла (1В = 100А)
#define ADC_TEMP_SCALE      ((3.3f / 4095.0f) * 100.0f) // Линейный термодатчик 10мВ/°C
#define PWM_MAX_TICKS       1000                      // ARR период TIM1 (100 кГц)
#define DT                  0.001f                    // Шаг дискретизации 1 мс

Welder_t Welder;
WelderAlarms_t WelderAlarms = { .all_flags = 0 };
uint32_t current_selected_mode = 0;

float cfg_hot_start_pct = 40.0f;
uint32_t cfg_hot_start_time_ms = 400;
float cfg_arc_force_pct = 30.0f;

static float kp_active = 0.8f;
static float ki_active = 15.0f;

extern TIM_HandleTypeDef htim1;
extern uint16_t adc_dma_buffer[4]; // Четкий массив на 4 канала из main.c

uint8_t ui_edit_target = 0; // По умолчанию при старте крутим Ток
void Welder_Init(void) {
    Welder.state = STATE_IDLE;
    Welder.current_setpoint = 90.0f;
    Welder.voltage_setpoint = 75.0f; // Дефолтное напряжение ХХ = 75 Вольт
    Welder.pid_integral = 0.0f;
    Welder_Hardware_Disable(); // При старте мост гарантированно заблокирован
}

void Welder_Hardware_Disable(void) {
    // HIGH открывает N-канальный ключ, прижимая пин SS UCC3895 к земле
    HAL_GPIO_WritePin(UCC_DISABLE_GPIO_Port, UCC_DISABLE_Pin, GPIO_PIN_SET); 
    TIM1->CCR1 = 0; // Гасим управляющее напряжение фазы в ноль
}

void Welder_Hardware_Enable(void) {
    if (WelderAlarms.all_flags == 0) {
        HAL_GPIO_WritePin(UCC_DISABLE_GPIO_Port, UCC_DISABLE_Pin, GPIO_PIN_RESET); 
    }
}

// Вызывается из stm32f4xx_it.c строго каждые 1.000 мс (Таймер TIM2)
void Welder_Process_1ms(void) {
    // 1. Оцифровка каналов АЦП из DMA массива
    Welder.voltage_filtered = (float)adc_dma_buffer[0] * ADC_VOLTAGE_SCALE;
    Welder.current_filtered = (float)adc_dma_buffer[1] * ADC_CURRENT_SCALE;
    Welder.temp_radiator    = (float)adc_dma_buffer[2] * ADC_TEMP_SCALE;
    
    // Замер питания драйверов
    float v_driver_15v = (float)adc_dma_buffer[3] * (3.3f / 4095.0f) * 5.0f; 
    if (v_driver_15v < 13.2f) WelderAlarms.low_15v = 1;
    if (Welder.temp_radiator > 85.0f) WelderAlarms.ntc_radiator = 1;

    // Проверка флага аппаратного триггера Break таймера TIM1 (Вход PB12)
    if (__HAL_TIM_GET_FLAG(&htim1, TIM_FLAG_BREAK) != RESET) {
        WelderAlarms.hardware_break = 1;
    }
    WelderAlarms.all_flags = 0;
    if (WelderAlarms.all_flags != 0) {
        Welder.state = STATE_ALARM;
    }

    // 2. Выбор динамики ПИД под физику процесса
    if (current_selected_mode == 0) {
        kp_active = 0.8f;   // MMA: мягкая дуга
        ki_active = 15.0f;
    } else {
        kp_active = 2.5f;   // TIG: мгновенная реакция при подъеме вольфрама
        ki_active = 40.0f;
    }

    // 3. Конечный автомат ВАХ сварочного процесса
    switch (Welder.state) {
        case STATE_IDLE:
            Welder_Hardware_Enable();
            Welder.pid_integral = 0.0f;
            
            if (current_selected_mode == 0) { // MMA
                Welder.current_active = 20.0f; 
                if (Welder.voltage_filtered < 30.0f && Welder.current_filtered > 15.0f) {
                    Welder.state = STATE_HOT_START;
                    Welder.state_timer = 0;
                }
            } else { // Lift-TIG
                Welder.current_active = 12.0f; // Безопасное дежурное касание
                if (Welder.voltage_filtered > 15.0f) { // Оторвали горелку
                    Welder.state = STATE_WELDING;
                }
            }
            break;

        case STATE_HOT_START:
            Welder.current_active = Welder.current_setpoint * (1.0f + (cfg_hot_start_pct / 100.0f)); 
            Welder.state_timer++;
            
            if (Welder.state_timer > cfg_hot_start_time_ms) {
                Welder.state = STATE_WELDING;
            }
            if (Welder.voltage_filtered < 12.0f && Welder.state_timer > 300) {
                Welder.state = STATE_ANTI_STICK;
                Welder.state_timer = 0;
            }
            break;

        case STATE_WELDING:
            Welder.current_active = Welder.current_setpoint;
            
            // Динамический Форсаж дуги (Arc Force) для ММА
            if (current_selected_mode == 0 && Welder.voltage_filtered < 18.0f) {
                float force_factor = (18.0f - Welder.voltage_filtered) / 18.0f;
                Welder.current_active += Welder.current_setpoint * (cfg_arc_force_pct / 100.0f) * force_factor;
            }

            // Залипание -> Переход в Anti-Stick
            if (Welder.voltage_filtered < 12.0f) {
                Welder.state_timer++;
                if (Welder.state_timer > 1000) { // Держится КЗ дольше 1 сек
                    Welder.state = STATE_ANTI_STICK;
                    Welder.state_timer = 0;
                }
            } else {
                Welder.state_timer = 0;
            }
            
            // Обрыв дуги
            if ((current_selected_mode == 0 && Welder.voltage_filtered > 55.0f) ||
                (current_selected_mode == 1 && Welder.voltage_filtered > 40.0f)) {
                Welder.state = STATE_IDLE;
            }
            break;

        case STATE_ANTI_STICK:
            Welder.current_active = 15.0f; // Падение тока до минимума
            Welder.pid_integral = 0.0f;
            if (Welder.voltage_filtered > 22.0f) { // Электрод оторвали
                Welder.state = STATE_IDLE;
            }
            break;

        case STATE_ALARM:
        default:
            Welder_Hardware_Disable();
            return;
    }

    // 4. Расчет ПИД-регулятора тока
    float error = Welder.current_active - Welder.current_filtered;
    
    Welder.pid_integral += error * DT;
    if (Welder.pid_integral > 60.0f)  Welder.pid_integral = 60.0f; // Anti-Windup лимиты
    if (Welder.pid_integral < -60.0f) Welder.pid_integral = -60.0f;

    float control_output = (kp_active * error) + (ki_active * Welder.pid_integral);

    if (control_output > 95.0f) control_output = 95.0f; // Ограничение максимального сдвига фаз
    if (control_output < 0.0f)  control_output = 0.0f;

    // 5. Передача уставки ШИМ в таймер TIM1 (Управление UCC3895)
    uint32_t ccr_value = (uint32_t)((control_output / 100.0f) * (float)PWM_MAX_TICKS);
    TIM1->CCR1 = ccr_value;
}