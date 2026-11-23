/* USER CODE BEGIN Header */
#ifndef INC_WELDER_H_
#define INC_WELDER_H_

#include "main.h"

// Состояния силового автомата ВАХ
typedef enum {
    STATE_IDLE = 0,         // Холостой ход
    STATE_HOT_START,        // Поджиг дуги (наброс тока)
    STATE_WELDING,          // Стабильная сварка (работа ПИД + Arc Force)
    STATE_ANTI_STICK,       // Электрод залип (ток в минимум)
    STATE_ALARM             // Авария (мост жестко заблокирован)
} WelderState_t;

// Битовые флаги аварийной системы по приоритетам
typedef union {
    uint16_t all_flags;
    struct {
        uint16_t low_15v       : 1; // Питание драйверов < 13.2В
        uint16_t ntc_radiator  : 1; // Перегрев силового радиатора (>85°C)
        uint16_t ntc_transform : 1; // Перегрев трансформатора (>85°C)
        uint16_t ntc_sensor_err: 1; // Обрыв или КЗ линий NTC
        uint16_t fan_failed    : 1; // Вентилятор заклинил или оборван
        uint16_t hardware_break: 1; // Аппаратный сверхток (Вход TIM1_BKIN)
    };
} WelderAlarms_t;

// Основная управляющая структура
typedef struct {
    WelderState_t state;
    float current_setpoint;  // Уставка пользователя (А)
    float voltage_setpoint;  // НОВОЕ: Уставка напряжения пользователи (В)
    float current_active;    // Целевой ток в текущий момент (А)
    float current_filtered;  // Реальный ток с АЦП (А)
    float voltage_filtered;  // Реальное напряжение с АЦП (В)
    float temp_radiator;     // Температура радиатора (°C)
    float temp_transformer;  // Температура транса (°C)
    uint32_t state_timer;    // Счетчик мс для тайм-аутов состояний
    float pid_integral;      // Интеграл ПИД-регулятора
} Welder_t;

// Глобальные переменные
extern uint8_t ui_edit_target; // 0 - ТОК, 1 - НАПРЯЖЕНИЕ
extern Welder_t Welder;
extern WelderAlarms_t WelderAlarms;
extern uint32_t current_selected_mode; // 0 - MMA, 1 - TIG

// Настройки инженерного меню
extern float cfg_hot_start_pct;
extern uint32_t cfg_hot_start_time_ms;
extern float cfg_arc_force_pct;

// Прототипы функций
void Welder_Init(void);
void Welder_Process_1ms(void);
void Welder_Hardware_Disable(void);
void Welder_Hardware_Enable(void);

#endif /* INC_WELDER_H_ */
/* USER CODE END Header */
