/* USER CODE BEGIN Header */
#ifndef INC_COOLING_H_
#define INC_COOLING_H_

#include "main.h"

/**
 * @brief  Низкоуровневая инициализация таймера ШИМ вентилятора (TIM11_CH1)
 */
void Cooling_Init(void);
/**
 * @brief  Потокобезопасный фоновый контроль температуры и кулера.
 * @note   Вызывается в главном суперцикле while(1) внутри main.c
 */
void Cooling_Process(void);

#endif /* INC_COOLING_H_ */
/* USER CODE END Header */