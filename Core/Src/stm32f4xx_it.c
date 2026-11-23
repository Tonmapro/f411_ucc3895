/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mma_fsm.h"
#include "signal_processing.h" // Если там лежат функции получения токов/напряжений
#include "hwd_ucc3895.h"       // global_fault_flag, UCC3895_Disable
#include "stm32f4xx_ll_tim.h" // <-- ОБЯЗАТЕЛЬНО ДОБАВИТЬ ЭТУ СТРОКУ!
#include "stm32f4xx_ll_dma.h" // <-- И ЭТУ (чтобы далее не ругался на флаг
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static volatile uint32_t s_system_ms_ticks = 0U; /* Системное время 1 мс (SysTick) */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */
/**
 * @brief Прототипы асинхронных регистровых обработчиков прерываний проекта (Чистый LL)
 */
extern void OLED_DMA_IRQHandler_Callback(void);     // Быстрый своп буферов OLED
extern void Signal_Processing_DMA_HT_Callback(void); // Полубуфер АЦП оверсэмплинга
extern void Signal_Processing_DMA_TC_Callback(void); // Полный буфер АЦП оверсэмплинга
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
  s_system_ms_ticks++; // Системный тик 1 мс (LL_Init1msTick уже настроил SysTick на 1 мс)
  /* USER CODE END SysTick_IRQn 0 */

  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */
   // Проверяем флаг прерывания по обновлению (переполнению) таймера
  if (LL_TIM_IsActiveFlag_UPDATE(TIM2) != RESET)
  {
    LL_TIM_ClearFlag_UPDATE(TIM2); // Обязательно вручную сбрасываем флаг!
    
    /* 
     * Получаем отфильтрованные физические значения кодов АЦП.
     * Функции берутся из вашего модуля signal_processing.c
     */
    uint16_t v_adc = Signal_Processing_Get_Voltage(); 
    uint16_t c_adc = Signal_Processing_Get_Current();

    /* 
     * ВЫЗОВ КОНЕЧНОГО АВТОМА СВАРКИ 
     * Выполняется жестко каждые 50 микросекунд в реальном времени!
     */
    FSM_MMA_Process(v_adc, c_adc);
  }

  /* USER CODE END TIM2_IRQn 0 */
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream2 global interrupt.
  */
void DMA2_Stream2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream2_IRQn 0 */
  OLED_DMA_IRQHandler_Callback();
   /* 
   * 2. ЖЕСТКИЙ ИНЖЕНЕРНЫЙ МАНЕВР (return):
   * Мы принудительно заставляем процессор немедленно выйти из прерывания.
   * Код HAL, расположенный ниже, никогда не выполнится, даже если CubeMX восстановит его.
   */
  return;
  /* USER CODE END DMA2_Stream2_IRQn 0 */
  /* USER CODE BEGIN DMA2_Stream2_IRQn 1 */

  /* USER CODE END DMA2_Stream2_IRQn 1 */
}

/* USER CODE BEGIN 1 */
/* USER CODE BEGIN 1 */

/**
  * @brief  Системное время в миллисекундах (замена отсутствующего в FW LL_GetTick)
  */
uint32_t System_Get_Tick(void)
{
  return s_system_ms_ticks;
}

/**
  * @brief  АППАРАТНЫЙ ОБРАБОТЧИК АЦП И ОВЕРСЭМПЛИНГА СИЛОВОЙ ПЛАТЫ (Чистый LL)
  * @note   Функция дописана вручную, так как в шаблоне CubeMX прерывание стрима было выключено.
  *         Имя строго соответствует вектору прерывания DMA2 Stream 0 для ADC1.
  */
void DMA2_Stream0_IRQHandler(void)
{
  // 1. Проверяем прерывание половины передачи Ping-Pong буфера (Half Transfer)
  if (LL_DMA_IsActiveFlag_HT0(DMA2))
  {
      LL_DMA_ClearFlag_HT0(DMA2);           // Атомарная очистка флага в HIFCR за 1 такт
      Signal_Processing_DMA_HT_Callback(); // Разрешаем обрабатывать первую половину в ОЗУ
      Signal_Processing_Update();          // Оверсэмплинг + EMA над свежей половиной буфера
  }

  // 2. Проверяем прерывание полной передачи буфера (Transfer Complete)
  if (LL_DMA_IsActiveFlag_TC0(DMA2))
  {
      LL_DMA_ClearFlag_TC0(DMA2);           // Атомарная очистка флага в HIFCR за 1 такт
      Signal_Processing_DMA_TC_Callback(); // Разрешаем обрабатывать вторую половину в ОЗУ
      Signal_Processing_Update();          // Оверсэмплинг + EMA над свежей половиной буфера
  }

  // 3. Публикация для FSM (TIM2 ISR) и суперцикла: копирование float атомарно на Cortex-M4
  Signal_Processing_Publish();
}

/**
  * @brief АВАРИЙНЫЙ ВХОД UCC3895 (линия EXTI12 = пин PB12, спад уровня = авария)
  * @note  Приоритет 0 (высший): глушим мост быстрее любого другого события.
  *        Сброс аварии возможен только через UCC3895_Clear_Fault() после диагностики.
  */
void EXTI15_10_IRQHandler(void)
{
  if (LL_EXTI_IsActiveFlag_0_31(LL_EXTI_LINE_12))
  {
      LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_12); // Сброс флага линии PR12
      global_fault_flag = 1U;                  // Фиксируем аварию (latch до явного сброса)
      UCC3895_Disable();                       // SS/DISABLE -> GND за 1 такт (BSRR)
      FSM_MMA_Force_Fault();                   // Автомат в SYS_STATE_FAULT + V(упр) = 0
  }
}

/* USER CODE END 1 */
