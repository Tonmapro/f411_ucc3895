#include "mma_fsm.h"
#include "config.h"
#include "regulator.h"
#include "hwd_ucc3895.h"
#include "signal_processing.h"

/* Выделение памяти под контекст инвертора */
Inverter_DataType Inverter;

#define TICKS_IN_1_MS         20U
static uint32_t state_timer_ticks = 0U;
static uint32_t short_circuit_counter = 0U;
static uint32_t arc_restore_counter = 0U;

void FSM_MMA_Init(void)
{
    Inverter.state = SYS_STATE_STANDBY;
    Inverter.target_current = WELD_CURRENT_DEFAULT;
    Inverter.actual_current = 0U;
    Inverter.actual_voltage = 0U;
    Inverter.out_dac_value = 0U;
    Inverter.fault_code = 0U;

    state_timer_ticks = 0U;
    short_circuit_counter = 0U;
    arc_restore_counter = 0U;
    
    PI_Regulator_Reset();
    UCC3895_Set_Control_Voltage(0.0f);
    UCC3895_Disable();
}

void FSM_MMA_Process(uint16_t raw_voltage, uint16_t raw_current)
{
    // Атомарное сохранение для UI-задач
    Inverter.actual_voltage = raw_voltage;
    Inverter.actual_current = raw_current;

    /* 0. ГЛОБАЛЬНЫЙ БАРЬЕР БЕЗОПАСНОСТИ */
    if (Inverter.state == SYS_STATE_FAULT || UCC3895_Is_Fault_Active())
    {
        Inverter.state = SYS_STATE_FAULT;
        UCC3895_Disable();
        UCC3895_Set_Control_Voltage(0.0f);
        PI_Regulator_Reset();
        return;
    }

    /* 0.1 ДЕТЕКЦИЯ ОТСУТСТВИЯ ЖЕЛЕЗА (приоритет: 15В → NTC → работа).
       Grace-период: EMA-фильтры стартуют с 0 и сходятся ~40-80 мс; раньше
       этого момента коды недостоверны (0 < 200 = «ложный обрыв 15В»),
       поэтому проверки железа включаем только после FSM_STARTUP_GRACE_MS. */
    if (System_Get_Tick() >= FSM_STARTUP_GRACE_MS)
    {
        /* ПРИОРИТЕТ 1: Нет питания 15В → "220V-!" */
        if (Signal_Processing_Get_Filtered_Code(ADC_V15V_IDX) < (float)ADC_THRES_LOW_15V) {
            Inverter.state = SYS_STATE_FAULT;
            Inverter.fault_code = FAULT_CODE_LOW_15V;
            UCC3895_Disable();
            PI_Regulator_Reset();
            return;
        }
        /* ПРИОРИТЕТ 2: Обрыв NTC (код уходит к 4095) → "TSENSOR" */
        if (Signal_Processing_Get_Filtered_Code(ADC_TEMP_IDX) > 4000.0f ||
            Signal_Processing_Get_Filtered_Code(ADC_TEMP_DIO_IDX) > 4000.0f) {
            Inverter.state = SYS_STATE_FAULT;
            Inverter.fault_code = FAULT_CODE_SENSOR_OPEN;
            UCC3895_Disable();
            PI_Regulator_Reset();
            return;
        }
    }

    // Инкремент системного времени с защитой от заклинивания
    if (state_timer_ticks < 0xFFFFFFFFU) {
        state_timer_ticks++;
    }

    switch (Inverter.state)
    {
        case SYS_STATE_STANDBY:
            /*
             * РЕЖИМ ХОЛОСТОГО ХОДА (ОCV) — п.5:
             * Мост ВКЛЮЧЕН, ПИ-регулятор держит малый дежурный ток MMA_STANDBY_CURR (10 А).
             * Без нагрузки ток = 0, ошибка положительна, выход регулятора уходит в
             * потолок 3.3 В -> на выходе инвертора устанавливается напряжение ХХ.
             * Оператор получает ОCV для поджига; при касании регулятор за 1 тик (50 мкс)
             * гасит фазовый сдвиг и удерживает безопасные 10 А.
             * Плавный разгон силовой части обеспечивает аппаратный soft-start UCC3895
             * (заряд внешнего SS-конденсатора клампит EAOUT независимо от нашего ЦАП).
             */
            UCC3895_Enable();
            PI_Regulator_Set_Setpoint((float)MMA_STANDBY_CURR);

            /* Факт касания: напряжение упало с уровня ХХ и пошёл ток дуги */
            if (raw_voltage < ADC_THRES_ARC_STRIKE && raw_current > 100U)
            {
                short_circuit_counter++;
                if (short_circuit_counter >= (2U * TICKS_IN_1_MS)) // Фильтр 2 мс
                {
                    short_circuit_counter = 0U;
                    state_timer_ticks = 0U;
                    /* Разводка по режиму: MMA -> Hot Start (+50%), Lift-TIG -> ожидание
                       отрыва вольфрама. Мост уже включен, ПИ продолжает работу без сброса */
                    Inverter.state = (current_selected_mode == 1U) ? SYS_STATE_TIG_LIFT
                                                                   : SYS_STATE_HOT_START;
                }
            }
            else
            {
                if (short_circuit_counter > 0U) short_circuit_counter--;
            }
            break;

        case SYS_STATE_HOT_START:
            {
                /* Горячий старт: +50% к току уставки */
                uint32_t calc_current = ((uint32_t)Inverter.target_current * 150U) / 100U;
                if (calc_current > WELD_CURRENT_MAX) calc_current = WELD_CURRENT_MAX;
                
                PI_Regulator_Set_Setpoint((float)calc_current);
            }

            if (state_timer_ticks >= (MMA_HOT_START_TIME_MS * TICKS_IN_1_MS))
            {
                state_timer_ticks = 0U;
                Inverter.state = SYS_STATE_WELDING;
            }
            
            /* Если при поджиге электрод намертво прилип */
            if (raw_voltage < ADC_THRES_ANTI_STICK)
            {
                short_circuit_counter++;
                if (short_circuit_counter >= (200U * TICKS_IN_1_MS)) // 200 мс на поджиг
                {
                    short_circuit_counter = 0U;
                    state_timer_ticks = 0U;
                    Inverter.state = SYS_STATE_ANTI_STICK;
                }
            }
            else
            {
                if (short_circuit_counter > 0U) short_circuit_counter--;
            }
            break;

        case SYS_STATE_WELDING:
            if (current_selected_mode == 1U) {
                /* ==================== LIFT-TIG ====================
                 * Плавный подъем тока (up-slope TIG_UPSLOPE_MS) от дежурных 10 А
                 * до уставки после зажигания дуги. Касание вольфрамом сварочной
                 * ванны (V~0В) — штатная ситуация: форсаж/анти-стик MMA не
                 * применяются, дуга восстанавливается при отрыве. */
                uint32_t upslope_ticks = (uint32_t)TIG_UPSLOPE_MS * TICKS_IN_1_MS;
                uint32_t ramp_t = (state_timer_ticks < upslope_ticks) ? state_timer_ticks
                                                                      : upslope_ticks;
                float tig_sp = (float)MMA_STANDBY_CURR +
                               ((float)Inverter.target_current - (float)MMA_STANDBY_CURR) *
                               ((float)ramp_t / (float)upslope_ticks);
                PI_Regulator_Set_Setpoint(tig_sp);

                /* Обрыв дуги (горелку убрали): V = ОCV, ток = 0 */
                if (raw_voltage > ADC_THRES_ARC_BREAK && raw_current < 100U)
                {
                    arc_restore_counter++;
                    if (arc_restore_counter >= (100U * TICKS_IN_1_MS))
                    {
                        arc_restore_counter = 0U;
                        Inverter.state = SYS_STATE_STANDBY;
                    }
                }
                else
                {
                    if (arc_restore_counter > 0U) arc_restore_counter--;
                }
            } else {
                /* ==================== ММА ==================== */
                PI_Regulator_Set_Setpoint((float)Inverter.target_current);

                /* Капля расплавленного металла замыкает дуговой промежуток */
                if (raw_voltage < ADC_THRES_ARC_FORCE)
                {
                    short_circuit_counter++;
                    if (short_circuit_counter >= (10U * TICKS_IN_1_MS)) // 10 мс стабильного КЗ капли
                    {
                        short_circuit_counter = 0U;
                        state_timer_ticks = 0U;
                        Inverter.state = SYS_STATE_ARC_FORCE;
                    }
                }
                /* Разрыв дуги: напряжение вернулось к уровню ХХ И ток упал почти до нуля.
                   Пара условий не зависит от точного значения ОCV и отсекает ложные
                   срабатывания при растяжке дуги (V растет, но ток еще течет) */
                else if (raw_voltage > ADC_THRES_ARC_BREAK && raw_current < 100U)
                {
                    arc_restore_counter++;
                    if (arc_restore_counter >= (100U * TICKS_IN_1_MS))
                    {
                        arc_restore_counter = 0U;
                        Inverter.state = SYS_STATE_STANDBY;
                    }
                }
                else
                {
                    if (short_circuit_counter > 0U) short_circuit_counter--;
                    if (arc_restore_counter > 0U) arc_restore_counter--;
                }
            }
            break;

        case SYS_STATE_ARC_FORCE:
            {
                /* Форсаж дуги: наброс +30% тока для выталкивания капли */
                uint32_t calc_current = ((uint32_t)Inverter.target_current * 130U) / 100U;
                if (calc_current > WELD_CURRENT_MAX) calc_current = WELD_CURRENT_MAX;
                
                PI_Regulator_Set_Setpoint((float)calc_current);
            }

            if (raw_voltage >= ADC_THRES_ARC_FORCE)
            {
                arc_restore_counter++;
                if (arc_restore_counter >= (5U * TICKS_IN_1_MS)) 
                {
                    arc_restore_counter = 0U;
                    Inverter.state = SYS_STATE_WELDING;
                }
            }
            else if (raw_voltage < ADC_THRES_ANTI_STICK)
            {
                short_circuit_counter++;
                if (short_circuit_counter >= (MMA_ANTI_STICK_TIME_MS * TICKS_IN_1_MS))
                {
                    short_circuit_counter = 0U;
                    state_timer_ticks = 0U;
                    Inverter.state = SYS_STATE_ANTI_STICK;
                }
            }
            else
            {
                if (short_circuit_counter > 0U) short_circuit_counter--;
                if (arc_restore_counter > 0U) arc_restore_counter--;
            }
            break;

        case SYS_STATE_TIG_LIFT:
            /*
             * LIFT-TIG: вольфрам касается детали, через КЗ течет дежурный ток 10 А.
             * Отрыв горелки поджигает дугу: напряжение подскакивает выше порога КЗ
             * (ADC_THRES_TIG_ARC ~7.5В), но остается ниже ОCV. Подтверждение 20 мс.
             */
            PI_Regulator_Set_Setpoint((float)MMA_STANDBY_CURR);

            /* Дуга Lift горит: V выше порога КЗ И ток течёт (~10 А ≈ 205 кодов).
               Важно: ОCV (~60В) тоже выше ADC_THRES_TIG_ARC, поэтому БЕЗ проверки
               тока "быстро убранная горелка" (V=ОCV, I=0) ошибочно трактовалась
               как зажигание и через 20 мс уводила в WELDING без дуги
               (найдено хост-симуляцией FSM, см. Docs/work_report.md). */
            if (raw_voltage > ADC_THRES_TIG_ARC && raw_current > 100U)
            {
                arc_restore_counter++;
                if (arc_restore_counter >= (20U * TICKS_IN_1_MS)) /* Дуга устойчива 20 мс */
                {
                    arc_restore_counter = 0U;
                    state_timer_ticks = 0U; /* Старт отсчета up-slope в WELDING */
                    Inverter.state = SYS_STATE_WELDING;
                }
            }
            else if (raw_voltage > ADC_THRES_ARC_BREAK && raw_current < 100U)
            {
                /* Горелку убрали быстро, дуга не зажглась: V = ОCV, ток = 0 */
                arc_restore_counter = 0U;
                Inverter.state = SYS_STATE_STANDBY;
            }
            else
            {
                if (arc_restore_counter > 0U) arc_restore_counter--;
                /* Защита от «залипания» вольфрама в контакте (КЗ) без зажигания:
                   бесконечно держать дежурный ток токопроводящей связи вредно —
                   возвращаемся на ХХ и ждём повторного касания */
                if (state_timer_ticks >= (TIG_STICK_TIMEOUT_MS * TICKS_IN_1_MS))
                {
                    arc_restore_counter = 0U;
                    Inverter.state = SYS_STATE_STANDBY;
                }
            }
            break;

        case SYS_STATE_ANTI_STICK:
            /* 
             * ИСПРАВЛЕНИЕ: Циклический сброс таймера для исключения зависания 
             * при долгом удержании короткого замыкания электродом.
             */
            if (state_timer_ticks >= (400U * TICKS_IN_1_MS)) {
                state_timer_ticks = 0U;
            }

            /* Короткий измерительный импульс 10 мс */
            if (state_timer_ticks < (10U * TICKS_IN_1_MS))
            {
                UCC3895_Enable(); 
                PI_Regulator_Set_Setpoint((float)MMA_ANTI_STICK_CURR); 
                
                if (raw_voltage > ADC_THRES_ARC_FORCE)
                {
                    PI_Regulator_Reset();
                    state_timer_ticks = 0U;
                    Inverter.state = SYS_STATE_STANDBY;
                }
            }
            else
            {
                /* 
                 * ИСПРАВЛЕНИЕ: Перед полным глушением гасим интегратор регулятора 
                 * и физически обнуляем управляющее напряжение ШИМ.
                 */
                UCC3895_Disable(); 
                UCC3895_Set_Control_Voltage(0.0f);
                PI_Regulator_Reset();
                PI_Regulator_Set_Setpoint(0.0f);
            }
            break;

        default:
            Inverter.state = SYS_STATE_FAULT;
            break;
    }

    /* ================================================================
     * ЗАМЫКАНИЕ КОНТУРА ТОКА (п.12):
     * ПИ-регулятор выполняется каждый тик 20 кГц сразу после того, как
     * автомат обновил уставку. Коды АЦП -> Амперы по масштабу датчика
     * (4095 кодов = CURRENT_SCALE А), выход в Вольтах уходит в ШИМ-ЦАП
     * TIM4 -> внешний ФНЧ -> EAP UCC3895.
     *
     * В состояниях с выключенным мостом (FAULT-барьер выше вышел по return;
     * фаза сна ANTI_STICK) интегратор сброшен и уставка 0 -> регулятор
     * выдает 0 В, а SS/DISABLE в любом случае аппаратно блокирует выходы.
     * ================================================================ */
    float i_amps = (float)raw_current * CURRENT_AMPS_PER_CODE; /* коды АЦП -> Амперы */
    float u_ctrl = PI_Regulator_Update(i_amps);                /* 0.0 .. 3.3 В */
    UCC3895_Set_Control_Voltage(u_ctrl);

    /* Диагностическое значение заполнения (шкала 0..1000, как у кулера) */
    Inverter.out_dac_value = (uint16_t)((u_ctrl * 1000.0f) / 3.3f);
}

void FSM_MMA_Force_Fault(void)
{
    Inverter.state = SYS_STATE_FAULT;
    UCC3895_Disable(); 
    UCC3895_Set_Control_Voltage(0.0f);
    PI_Regulator_Reset();
}

mma_state_t FSM_MMA_Get_State(void)
{
    return (mma_state_t)Inverter.state;
}

void FSM_MMA_Set_User_Current(uint16_t new_current_setpoint)
{
    if (new_current_setpoint < WELD_CURRENT_MIN) Inverter.target_current = WELD_CURRENT_MIN;
    else if (new_current_setpoint > WELD_CURRENT_MAX) Inverter.target_current = WELD_CURRENT_MAX;
    else Inverter.target_current = new_current_setpoint;
}
