#include "config.h"
#include "regulator.h"
#include <string.h>

/* Глобальный статический экземпляр контроллера (volatile защищает от кеширования GCC) */
volatile PI_Controller_t pid;

void PI_Regulator_Init(float kp, float ki, float out_min, float out_max)
{
    pid.Kp = kp;
    pid.Ki = ki * 0.000050f; // Шаг дискретизации 50 мкс (20 кГц)
    pid.out_min = out_min;
    pid.out_max = out_max;
    
    PI_Regulator_Reset();
}

void PI_Regulator_Set_Setpoint(float setpoint)
{
    /* Атомарно для компилятора обновляем уставку */
    pid.setpoint = setpoint;
}

void PI_Regulator_Reset(void)
{
    pid.integrator = 0.0f;
}

float PI_Regulator_Update(float current_feedback)
{
    // 1. Расчет текущей ошибки системы в физических Амперах
    float error = pid.setpoint - current_feedback;

    // 2. Расчет пропорциональной составляющей
    float p_term = pid.Kp * error;

    // 3. Предварительный расчет нового значения интегратора
    // pid.Ki уже умножен на 50 мкс в Init, здесь просто быстрое сложение
    float next_integrator = pid.integrator + (pid.Ki * error);

    // 4. Ограничение интегратора (Anti-Windup) по физическим границам ШИМ
    if (next_integrator > pid.out_max) next_integrator = pid.out_max;
    if (next_integrator < pid.out_min) next_integrator = pid.out_min;
    pid.integrator = next_integrator;

    // 5. Формирование полного выходного напряжения
    float output = p_term + pid.integrator;

    // 6. Финальное жесткое ограничение выхода
    if (output > pid.out_max) output = pid.out_max;
    if (output < pid.out_min) output = pid.out_min;

    return output;
}

