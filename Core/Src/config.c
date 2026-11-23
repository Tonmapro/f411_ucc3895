//#ifndef CORE_INC_CONFIG_H_
//#define CORE_INC_CONFIG_H_

#include <stdint.h>
#include "main.h"
#include "config.h"

/* ========================================================================== */
/*                      ГЕОМЕТРИЯ И СТРУКТУРА АЦП МАТЕМАТИКИ                 */
/* ========================================================================== */
/* Файл-легаси: все константы перенесены в Core/Inc/config.h (источник истины).
   Активные #define здесь УДАЛЕНЫ — они конфликтовали с config.h (ADC_CHANNELS). */

#if 0 // ЛЕГАСИ-БЛОК (не активен): дублировал config.h со старыми значениями
#define ADC_CHANNELS            5U   
#define ADC_OVERSAMPLING        16U  
#define ADC_BUFFER_SIZE         (ADC_CHANNELS * ADC_OVERSAMPLING) 
#define PING_PONG_BUFF_SIZE     (ADC_BUFFER_SIZE * 2U)

//typedef enum {
//    ADC_CURRENT_IDX   = 0U,          
//    ADC_VOLTAGE_IDX   = 1U,          
//    ADC_V15V_IDX      = 2U,          
//    ADC_TEMP_IDX      = 3U,          
//    ADC_FAN_CURR_IDX  = 4U           
//} Adc_Channel_Index_t;

/* ========================================================================== */
/*                       ФИЗИЧЕСКИЕ ЛИМИТЫ СИЛОВОЙ ЧАСТИ                      */
/* ========================================================================== */
#define WELD_CURRENT_MIN        20U  
#define WELD_CURRENT_MAX        180U 
#define WELD_CURRENT_DEFAULT    90U  

#define MMA_HOT_START_TIME_MS   700U 
#define MMA_ANTI_STICK_TIME_MS  400U 
#define MMA_ANTI_STICK_CURR     10U  

#define ADC_THRES_NO_LOAD       2459U 
#define ADC_THRES_ARC_FORCE     778U  
#define ADC_THRES_ANTI_STICK    410U  

#define ADC_THRES_TEMP_CRIT      1200U 
#define ADC_THRES_TEMP_WARN      1800U 
#define ADC_THRES_TEMP_OFF       2800U 

#define ADC_TSENSOR_SHORT        50U   
#define ADC_TSENSOR_OPEN         4000U 

#define ADC_FAN_CURR_SHORT       3500U 
#define ADC_FAN_CURR_OPEN        40U   

#define FAULT_CODE_OVERHEAT      0x01U 
#define FAULT_CODE_LOW_15V       0x02U 
#define FAULT_CODE_FAN_BLOCKED   0x03U 
#define FAULT_CODE_OVERCURRENT   0x04U 
#define FAULT_CODE_SENSOR_OPEN   0x05U 
#endif // ЛЕГАСИ-БЛОК

/* ========================================================================== */
/*           СТРУКТУРЫ ДАННЫХ (ПОРЯДОК ОБЪЯВЛЕНИЯ КРИТИЧЕН!)                  */
/* ========================================================================== */

/**
 * @brief КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Структура объявлена ПЕРВОЙ.
 * Теперь компилятор увидит её до того, как мы объявим extern переменные!
 */
//typedef struct {
//    uint16_t hot_start_time_ms;   
//    uint16_t hot_start_current_p; 
//    uint16_t arc_force_time_ms;   
//    uint16_t arc_force_current_p; 
//    uint16_t anti_stick_time_ms;  
//} Settings_DataType; 

//typedef enum {
//    SYS_STATE_STANDBY    = 0xA5U, 
//    SYS_STATE_HOT_START  = 0x5AU, 
//    SYS_STATE_WELDING    = 0x3CU, 
//    SYS_STATE_ARC_FORCE  = 0xC3U, 
//    SYS_STATE_ANTI_STICK = 0x96U, 
//    SYS_STATE_FAULT      = 0x69U  
//} mma_state_t;

//typedef struct {
//    volatile uint16_t target_current; 
//    volatile uint16_t actual_current; 
//    volatile uint16_t actual_voltage; 
//    volatile uint16_t out_dac_value;  
//    volatile mma_state_t state;       
//    volatile uint32_t fault_code;     
//} Inverter_DataType;

/* Объявляем внешние переменные для линкера */
//extern Inverter_DataType Inverter;
//extern Settings_DataType InverterSettings; 

//#endif /* CORE_INC_CONFIG_H_ */

