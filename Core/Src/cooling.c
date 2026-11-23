#include "tim.h"
#include "cooling.h"
#include "config.h"
#include "mma_fsm.h"
#include "signal_processing.h"

typedef enum {
    COOL_START_PURGE = 0xA5U, // Помехоустойчивые маркеры состояний кулера
    COOL_AUTOMATIC   = 0x5AU
} CoolMode_t;

static CoolMode_t cool_mode = COOL_START_PURGE;
static uint32_t purge_timer = 0U;
static uint8_t  fan_is_running = 0U;

void Cooling_Init(void) {
    /* Активируем счетчик таймера кулера (TIM11) на уровне Low-Layer */
    LL_TIM_EnableCounter(TIM11);
    LL_TIM_CC_EnableChannel(TIM11, LL_TIM_CHANNEL_CH1);
    
    cool_mode = COOL_START_PURGE;
    purge_timer = System_Get_Tick();
    
    /* Стартовая продувка: включаем кулер на 100% (ARR равен 1000) */
    LL_TIM_OC_SetCompareCH1(TIM11, 1000U);
}

void Cooling_Process(void) {
    uint32_t current_time = System_Get_Tick();

    /* 1. Безопасное чтение усредненных кодов АЦП из ядра цифровой фильтрации.
       Все 6 каналов оцифровываются: тепловая защита диодов и токовый контроль
       вентилятора активны (PA5/IN5 = NTC диодов, PB0/IN8 = датчик тока кулера). */
    uint16_t fan_curr_code  = (uint16_t)(Signal_Processing_Get_Filtered_Code(ADC_FAN_CURR_IDX) + 0.5f);
    uint16_t temp_igbt_code = (uint16_t)(Signal_Processing_Get_Filtered_Code(ADC_TEMP_IDX) + 0.5f);
    uint16_t temp_dio_code  = (uint16_t)(Signal_Processing_Get_Filtered_Code(ADC_TEMP_DIO_IDX) + 0.5f);

    /* Выбираем наиболее нагретый узел (для NTC-термисторов это НАИМЕНЬШИЙ код АЦП) */
    uint16_t max_active_temp_code = (temp_igbt_code < temp_dio_code) ? temp_igbt_code : temp_dio_code;

    /* 2. Аппаратный барьер термозащиты: если температура превысила критический порог.
       НЕ срабатываем, если:
       - идёт grace-период (EMA ещё не сошлась, коды 0 = «ложный перегрев»)
       - канал +15В вне нормы (<200 кодов): питание измерительных цепей (или общая
         шина заглушек на макете) отсутствует → все коды NTC тоже падают к 0, и без
         этой проверки выставлялся бы ЛОЖНЫЙ «HOT!!», маскируя истинную причину
         («220V-!»). Устраняет гонку приоритетов между cooling.c и mma_fsm.c —
         первым всегда выставляется правильный код аварии.
       - коды > 3500 (обрыв/несобранный датчик → TSENSOR выставляется FSM, не HOT)
       - уже активна другая авария (не перетираем fault_code от FSM) */
    if ((max_active_temp_code <= ADC_THRES_TEMP_CRIT) &&
        (Signal_Processing_Get_Filtered_Code(ADC_V15V_IDX) >= (float)ADC_THRES_LOW_15V) &&
        (System_Get_Tick() >= FSM_STARTUP_GRACE_MS) &&
        (temp_igbt_code < 3500U) &&
        (temp_dio_code < 3500U) &&
        (Inverter.fault_code == 0U)) { // Например, код упал ниже 1200 (перегрев)
        FSM_MMA_Force_Fault(); // Мгновенно глушим ШИМ UCC3895
    
        /* Точечно определяем виновника перегрева для вывода на дисплей */
        if (temp_igbt_code < temp_dio_code) {
        Inverter.fault_code = FAULT_CODE_OVERHEAT_IGBT; // [pdf_mC_QGf.pdf]
        } else {
            Inverter.fault_code = FAULT_CODE_OVERHEAT_DIO;  // [pdf_mC_QGf.pdf]
        }
    
        LL_TIM_OC_SetCompareCH1(TIM11, 1000U); // Врубаем вентилятор на 100%
        return;
    }

    /* 3. Логика стартовой продувки (3 секунды после включения автомата) */
    if (cool_mode == COOL_START_PURGE) {
        if ((current_time - purge_timer) > 3000U) {
            cool_mode = COOL_AUTOMATIC; // Переходим в штатный автомат контроля
        }
        
        /* Проверка исправности вентилятора при пуске (Обрыв цепи обмотки).
           Grace: код кулера в первые ~80 мс ещё 0 (EMA сходится) — не срабатываем */
        if ((fan_curr_code < ADC_FAN_CURR_OPEN) &&
            (System_Get_Tick() >= FSM_STARTUP_GRACE_MS) &&
            (Inverter.fault_code == 0U)) {
            FSM_MMA_Force_Fault();
            Inverter.fault_code = FAULT_CODE_FAN_BLOCKED;
        }
        return; 
    }

    /* 4. Логика автоматического регулирования скорости с гистерезисом */
    uint32_t pwm_duty = 0U;

    if (!fan_is_running) {
        /* Кулер стоял. Включаем, если код температуры упал ниже порога старта (стало горячее) */
        if (max_active_temp_code <= ADC_THRES_TEMP_OFF) {
            fan_is_running = 1U;
        }
    } else {
        /* Кулер вращается. Полное отключение только если остыл (код вырос выше гистерезиса) */
        // Гистерезис +300 кодов (~9°C при линейной шкале 120°C на полный код 4095):
        // отключение только когда NTC остыл заметно ниже порога включения
        if (max_active_temp_code > (ADC_THRES_TEMP_OFF + 300U)) {
            fan_is_running = 0U;
        }
    }

    /* 5. Математический пересчет коэффициента заполнения ШИМ кулера */
    if (fan_is_running) {
        if (max_active_temp_code <= ADC_THRES_TEMP_WARN) {
            /* Температура выше порога Warning (код <= 1800) -> Врубаем 100% оборотов */
            pwm_duty = 1000U;
        } else {
            /* Линейная интерполяция оборотов в диапазоне [40°C ... 60°C] */
            // Перевернутая NTC математика: чем меньше код, тем выше ШИМ
            float current_range = (float)(ADC_THRES_TEMP_OFF - max_active_temp_code);
            float total_range   = (float)(ADC_THRES_TEMP_OFF - ADC_THRES_TEMP_WARN);
            float factor = current_range / total_range;
            if (factor > 1.0f) factor = 1.0f;
            if (factor < 0.0f) factor = 0.0f;

            /* Стартовый ШИМ кулера ставим 35% (350 ед.), чтобы крыльчатка не гудела на месте */
            pwm_duty = 350U + (uint32_t)(factor * (1000.0f - 350.0f));
        }

        /* Постоянная защита по току в процессе работы (Заклинивание крыльчатки окалиной) */
        if (fan_curr_code > ADC_FAN_CURR_SHORT) {
            FSM_MMA_Force_Fault();
            Inverter.fault_code = FAULT_CODE_FAN_BLOCKED;
            pwm_duty = 0U; // Снимаем питание с заклинившего мотора
        }
    } else {
        pwm_duty = 0U; // Инвертор холодный, вентилятор полностью обесточен
    }

    /* Атомарная запись вычисленного значения заполнения в регистр таймера */
    LL_TIM_OC_SetCompareCH1(TIM11, pwm_duty);
}
