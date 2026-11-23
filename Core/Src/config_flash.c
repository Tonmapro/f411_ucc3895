#include "config_flash.h"
#include "config.h"     // Inverter, SYS_STATE_STANDBY, WELD_CURRENT_MIN/MAX

/* Аппаратные ключи разблокировки Flash-контроллера (RM0383, гл. 3) */
#define FLASH_KEY1               0x45670123U
#define FLASH_KEY2               0xCDEF89ABU

/* Режим аппарата: определен в encoder.c (переключение длинным нажатием) */
extern uint32_t current_selected_mode;

static uint32_t save_requested_time = 0U;
static uint8_t  save_pending = 0U;

/* --- Внутренние атомарные функции для работы с регистрами Flash --- */

static void Flash_Unlock(void) {
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static void Flash_Lock(void) {
    FLASH->CR |= FLASH_CR_LOCK;
}

static void Flash_WaitLastOp(void) {
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        /* Внимание: выборка команд из Flash во время операции стоит -
           весь ЦП (включая прерывания) замирает на время стирания/записи */
    }
}

/**
 * @brief Загрузка настроек при старте. Валидируются magic И версия формата:
 *        чистый сектор (0xFFFFFFFF) или чужая раскладка -> молча работаем на
 *        дефолтах FSM_MMA_Init (запись дефолтов произойдет после первого
 *        изменения уставки пользователем).
 */
void Config_Load(void) {
    const FlashConfig_t *flash_data = (const FlashConfig_t *)FLASH_SETTING_ADDRESS;

    if ((flash_data->magic == CONFIG_MAGIC_NUMBER) &&
        (flash_data->version == CONFIG_VERSION)) {
        if (flash_data->welder_mode <= 1U) {
            current_selected_mode = flash_data->welder_mode;
        }
        if ((flash_data->target_current >= WELD_CURRENT_MIN) &&
            (flash_data->target_current <= WELD_CURRENT_MAX)) {
            Inverter.target_current = flash_data->target_current;
        }
    }
}

/**
 * @brief Физическая запись настроек во Flash на чистых регистрах LL.
 * @note  БЕЗОПАСНОСТЬ: стирание сектора (~0.3-0.5 c для 16 КБ) останавливает
 *        выборку команд из Flash — замирает весь ЦП вместе с прерыванием
 *        контура тока (TIM2 20 кГц). Поэтому запись разрешена СТРОГО в
 *        состоянии SYS_STATE_STANDBY: мост держит ОCV аппаратно, дуги нет,
 *        задержка регулятора безопасна. При отказе pending сохраняется и
 *        Config_Check_Timeout() повторит попытку позже.
 */
void Config_Save(void) {
    if (Inverter.state != SYS_STATE_STANDBY) return; /* пишем только на холостом ходу */

    FlashConfig_t local_config;
    local_config.magic          = CONFIG_MAGIC_NUMBER;
    local_config.version        = CONFIG_VERSION;
    local_config.welder_mode    = (uint16_t)(current_selected_mode & 1U);
    local_config.target_current = Inverter.target_current;
    local_config.reserved       = 0xFFFFU;

    /* 1. Разблокируем контроллер Flash */
    Flash_Unlock();
    Flash_WaitLastOp();

    /* 2. Стирание сектора (RM0383, п. 3.6.2): PSIZE=x32 (VDD 2.7-3.6В), SNR=сектор */
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB | FLASH_CR_PSIZE);
    FLASH->CR |= FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_SER | ((FLASH_SETTING_SECTOR << FLASH_CR_SNB_Pos) & FLASH_CR_SNB);
    FLASH->CR |= FLASH_CR_STRT;
    Flash_WaitLastOp();
    FLASH->CR &= ~FLASH_CR_SER;

    /* 3. Пословная запись структуры (по 32 бита) */
    const uint32_t *source = (const uint32_t *)&local_config;
    __IO uint32_t *dest = (__IO uint32_t *)FLASH_SETTING_ADDRESS;

    for (uint32_t i = 0; i < (sizeof(FlashConfig_t) / 4U); i++) {
        /* Переводим Flash в режим программирования x32 */
        FLASH->CR &= ~(FLASH_CR_PG | FLASH_CR_PSIZE);
        FLASH->CR |= FLASH_CR_PG | FLASH_CR_PSIZE_1;

        dest[i] = source[i];
        Flash_WaitLastOp();

        /* Контроль ошибок записи (защита от записи / неверный адрес / параллелизм) */
        if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR)) != 0U) {
            FLASH->SR |= (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR);
            FLASH->CR &= ~FLASH_CR_PG;
            Flash_Lock();
            return; /* save_pending не сбрасываем: повтор по таймауту */
        }
    }

    /* 4. Выключаем режим программирования и блокируем Flash */
    FLASH->CR &= ~FLASH_CR_PG;
    Flash_Lock();
    save_pending = 0U;
}

/* Запрос на отложенное сохранение (вызывается из контекста энкодера/UI) */
void Config_Request_Save(void) {
    save_requested_time = System_Get_Tick();
    save_pending = 1U;
}

/* Проверка тайм-аута (вызывается из while(1) главного цикла) */
void Config_Check_Timeout(void) {
    if (save_pending != 0U) {
        if ((System_Get_Tick() - save_requested_time) > 3000U) {
            Config_Save(); /* 3 с тишены — фиксируем настройки во Flash */
        }
    }
}
