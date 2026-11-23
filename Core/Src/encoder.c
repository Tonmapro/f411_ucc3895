#include "encoder.h"
#include "config.h"
#include "config_flash.h"  // Config_Request_Save: отложенное сохранение уставки во Flash
#include "mma_fsm.h"

/* Круговой буфер истории шагов для мажоритарного фильтра */
#define ENCODER_WINDOW_SIZE    5U
static uint16_t encoder_history[ENCODER_WINDOW_SIZE] = {0};
static uint8_t  history_idx = 0U;

static uint16_t last_hardware_counter = 0U;
static uint16_t filtered_user_current = WELD_CURRENT_DEFAULT; // Стартуем с 90А [pdf_G_X6G-.pdf]

/* Глобальные флаги интерфейса для ui_manager.c */
uint8_t ui_edit_target = 0U;        // 0 = Регулируем Ток, 1 = Регулируем Вольты
uint32_t current_selected_mode = 0U; // 0 = MMA, 1 = TIG (определение перенесено из welder.c)

/* Переменные аппаратной кнопки энкодера с фильтрацией ВЧ дребезга дуги */
static uint8_t  button_click_flag = 0U;
static uint16_t button_debounce_counter = 0U;
static uint8_t  last_button_state = 1U; // pull-up (HIGH когда отпущена)

/* Длинное нажатие (>=2 c) — переключение режима MMA <-> Lift-TIG */
#define ENCODER_LONG_PRESS_MS  2000U
static uint32_t btn_press_start_ms = 0U; // Момент фиксации нажатия (System_Get_Tick)
static uint8_t  long_press_fired   = 0U; // Длинное нажатие уже отработано

void Encoder_Init(void) {
    /* Запускаем счетчик таймера TIM3 в режиме Encoder Mode (MX_TIM3_Init: PA6/PA7, X2) */
    LL_TIM_EnableCounter(TIM3);
    last_hardware_counter = (uint16_t)LL_TIM_GetCounter(TIM3);
    
    /* Заполняем буфер фильтра базовым значением тока по умолчанию */
    for (uint8_t i = 0U; i < ENCODER_WINDOW_SIZE; i++) {
        encoder_history[i] = WELD_CURRENT_DEFAULT; // [pdf_G_X6G-.pdf]
    }
    filtered_user_current = WELD_CURRENT_DEFAULT;
    button_click_flag = 0U;
    ui_edit_target = 0U;
}

/**
 * @brief  Высокоскоростная сеть сравнений для поиска медианы из 5 элементов.
 *         Очищает уставку от импульсных наводок силовой дуги за минимальное число тактов.
 */
static uint16_t Extract_Median_5(uint16_t *buffer) {
    uint16_t a = buffer[0], b = buffer[1], c = buffer[2], d = buffer[3], e = buffer[4];
    uint16_t tmp;
    
    if (a > b) { tmp = a; a = b; b = tmp; }
    if (c > d) { tmp = c; c = d; d = tmp; }
    if (a > c) { tmp = a; a = c; c = tmp; tmp = b; b = d; d = tmp; }
    if (b > c) { tmp = b; b = c; c = tmp; }
    if (e > c) {
        if (e < d) return e;
        return d;
    } else {
        if (e > b) return e;
        return b;
    }
}

void Encoder_Update_Value(void) {
    /* 1. Атомарное считывание регистра аппаратного счетчика TIM3 */
    uint16_t current_hardware_cnt = (uint16_t)LL_TIM_GetCounter(TIM3);
    
    if (current_hardware_cnt != last_hardware_counter) {
        // Вычисляем дельту с автоматическим учетом переполнения 16-битного регистра
        int16_t delta = (int16_t)(current_hardware_cnt - last_hardware_counter);
        
        /* Физический делитель шагов (сдвиг вправо): отсекает полушаги трещотки */
        int16_t steps = delta / 2; 
        
        if (steps != 0) {
            last_hardware_counter = current_hardware_cnt;
            
            /* Проверяем фокус ввода: что именно настраивает пользователь */
            if (current_selected_mode == 1) {
                // В режиме Lift-TIG изменяется строго только Сварочный Ток [pdf_K8I6_K.pdf]
                ui_edit_target = 0U;
            }

            if (ui_edit_target == 0U) {
                /* РЕГУЛИРОВКА СВАРОЧНОГО ТОКА */
                int32_t new_current = (int32_t)filtered_user_current + steps;
                
                // Жесткие барьеры из config.h
                if (new_current > (int32_t)WELD_CURRENT_MAX) new_current = WELD_CURRENT_MAX; // [pdf_G_X6G-.pdf]
                if (new_current < (int32_t)WELD_CURRENT_MIN) new_current = WELD_CURRENT_MIN; // [pdf_G_X6G-.pdf]
                
                /* Проталкиваем новое сырое значение в круговой буфер истории */
                encoder_history[history_idx] = (uint16_t)new_current;
                history_idx++;
                if (history_idx >= ENCODER_WINDOW_SIZE) history_idx = 0U;
                
                /* Фильтрация: отсекаем наводки сварочных кабелей */
                filtered_user_current = Extract_Median_5(encoder_history);
                
                /* Атомарно обновляем глобальный контекст для ПИ-регулятора */
                Inverter.target_current = filtered_user_current;

                /* Отложенное сохранение уставки: через 3 с после последнего
                   вращения ручки Config_Check_Timeout() запишет во Flash */
                Config_Request_Save();
            } 
            else {
                /* РЕГУЛИРОВКА НАПРЯЖЕНИЯ ХОЛОСТОГО ХОДА (MMA) */
                // Если в будущем добавите переменную Welder.voltage_setpoint,
                // шаг изменения "steps" будет инкрементировать её здесь через барьеры [50V ... 85V]
            }
        }
    }
    /* 2. ПОМЕХОЗАЩИЩЕННЫЙ ОПРОС КНОПКИ (Вход PB7 = ENC_BUTTON, подтяжка задана в MX_GPIO_Init).
       Перенастройка АЦП-пинов запрещена: PA1 = ADC1_IN1 (канал напряжения дуги)! */
    uint8_t current_btn_state = LL_GPIO_IsInputPinSet(ENC_BUTTON_GPIO_Port, ENC_BUTTON_Pin) ? 1U : 0U;
    
    if (current_btn_state == 0U && last_button_state == 1U) {
        /* Нажатие кнопки (Переход HIGH -> LOW). Накапливаем фильтр дребега */
        button_debounce_counter++;
        if (button_debounce_counter >= 50U) { // Требуется стабильное удержание 50 тактов
            button_click_flag = 1U;
            last_button_state = 0U;
            button_debounce_counter = 0U;
            btn_press_start_ms = System_Get_Tick();
            long_press_fired = 0U;

            /* Короткий клик: инвертируем цель редактирования (Ток <-> Вольты) */
            if (current_selected_mode == 0U) { // Только для ММА
                ui_edit_target = (ui_edit_target == 0U) ? 1U : 0U;
            }
        }
    } else if (current_btn_state == 1U && last_button_state == 0U) {
        /* Отпускание кнопки (Переход LOW -> HIGH) */
        button_debounce_counter++;
        if (button_debounce_counter >= 50U) {
            last_button_state = 1U;
            button_debounce_counter = 0U;
            long_press_fired = 0U;
        }
    } else if ((current_btn_state == 0U) && (last_button_state == 0U)) {
        /* УДЕРЖАНИЕ: длинное нажатие >= 2 c — переключение режима MMA <-> Lift-TIG.
           Отсчет по системному времени (мс), а не по скорости суперцикла */
        if ((long_press_fired == 0U) &&
            ((System_Get_Tick() - btn_press_start_ms) >= ENCODER_LONG_PRESS_MS)) {
            long_press_fired = 1U;
            current_selected_mode = (current_selected_mode == 0U) ? 1U : 0U;
            ui_edit_target = 0U;      // В TIG фокус всегда на токе
            Config_Request_Save();    // Режим тоже сохраняем во Flash
        }
    } else {
        /* Защита от случайных микросекундных импульсов наводок от дуги */
        if (button_debounce_counter > 0U) button_debounce_counter--;
    }
}

/**
 * @brief Принудительная установка уставки (синхронизация фильтра с Config_Load).
 * @note  Заполняет весь медианный буфер одним значением, чтобы фильтр мгновенно
 *        принял загруженную из Flash уставку без переходного процесса.
 */
void Encoder_Set_Value(uint16_t current) {
    if (current < WELD_CURRENT_MIN) current = WELD_CURRENT_MIN;
    if (current > WELD_CURRENT_MAX) current = WELD_CURRENT_MAX;

    filtered_user_current = current;
    for (uint8_t i = 0U; i < ENCODER_WINDOW_SIZE; i++) {
        encoder_history[i] = current;
    }
    history_idx = 0U;
    Inverter.target_current = current;
}

uint16_t Encoder_Get_Value(void) {
    return filtered_user_current;
}

uint8_t Encoder_Is_Pressed(void) {
    if (button_click_flag) {
        button_click_flag = 0U; // Автосброс флага при чтении
        return 1U;
    }
    return 0U;
}
