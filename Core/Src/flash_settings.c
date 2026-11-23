/**
 * @file    flash_settings.c
 * @brief   Хранение инженерных настроек во внутреннем flash МК
 *          (MMA_FSM_MODES.md §6 / §7 п.3): CRC-32 + слоты A/B + запись
 *          только из STANDBY с IWDG-огибающей и RAM-ожиданием BSY.
 *
 * ГЕОМЕТРИЯ (исправление против §6 доки): в §6 указан «сектор 11 @
 * 0x080E0000», но на STM32F411CE (512 КБ flash) существуют только сектора
 * 0..7; последние два сектора по 128 КБ — №6 (0x08040000) и №7 (0x08060000).
 * Они и стали слотами A/B: на F4 стирание возможно ТОЛЬКО целым сектором,
 * поэтому два слота внутри одного сектора не работали бы (стирание уничтожило
 * бы оба). Исполняемый код обязан помещаться ниже 0x08040000 — контролируется
 * по .map файлу после сборки.
 *
 * БЕЗОПАСНОСТЬ ЗАПИСИ (правила §6 — выполнены все):
 *   1) сохранение ТОЛЬКО из STANDBY без аварии; перед стиранием мост гасится
 *      (UCC3895_Disable + 0 В + reset ПИ-регулятора), прерывания запрещаются
 *      — иначе ISR TIM2 20 кГц в STANDBY-ветке вернёт UCC3895_Enable()
 *      через 50 мкс, и мост продолжит работать во время стирания;
 *   2) IWDG (~100 мс) временно расширяется до максимума (~4.1 с): стирание
 *      сектора 128 КБ занимает до ~2 с (DS), суперцикл заморожен на это
 *      время; кормление дополнительно выполняется прямо из цикла ожидания
 *      BSY (RAM-код);
 *   3) ожидание BSY живёт в .RamFunc (линкер кладёт секцию в RAM, startup
 *      копирует её вместе с .data): ядру не нужно выбирать инструкции из
 *      занятой стиранием flash-шины.
 *
 * ЦЕЛОСТНОСТЬ: magic + version + монотонный seq + CRC-32 (отражённый
 * IEEE-802.3, poly 0xEDB88320 — как у zlib) над полезной нагрузкой.
 * При восстановлении берётся валидный слот со старшим seq; невалидный CRC
 * или пустой (0xFF) слот -> дефолты config.c. Аппаратный CRC-модуль в .ioc
 * не включен, поэтому CRC-32 программный (12 байт payload — копеечная цена).
 */

#include <stddef.h>
#include "flash_settings.h"
#include "config.h"
#include "main.h"          /* FLASH/IWDG регистры, __disable_irq, CMSIS */
#include "hwd_ucc3895.h"   /* UCC3895_Disable / Is_Fault_Active         */
#include "regulator.h"     /* PI_Regulator_Reset                        */

/* ========================================================================== */
/*                     ГЕОМЕТРИЯ И ФОРМАТ ХРАНЕНИЯ                            */
/* ========================================================================== */

#define FSET_SLOT_A_ADDR     0x08040000UL /**< Слот A: сектор 6 (128 КБ) */
#define FSET_SLOT_B_ADDR     0x08060000UL /**< Слот B: сектор 7 (128 КБ) */
#define FSET_SECTOR_A_NUM    6U           /**< Номер сектора слота A для FLASH_CR.SNB */
#define FSET_SECTOR_B_NUM    7U           /**< Номер сектора слота B для FLASH_CR.SNB */

#define FSET_MAGIC           0x53545331UL /**< "STS1" — метка валидной записи */
/* Версия 2 (редизайн UI 01.09.2026): в payload вместо reserved — уставка
 * сварочного тока weld_current_a. Версия 3 (02.09.2026): добавлены 4 флага
 * включения функций — hot_start_en, arc_force_en, anti_stick_en, vrd_en
 * (payload 12 -> 16 байт). Записи v2/v1 автоматически считаются невалидными
 * (version не совпадёт) — аппарат один раз стартует на дефолтах config.c. */
#define FSET_VERSION         3UL          /**< Версия формата записи */

/* Ключи разблокировки/команды IWDG (RM0383) */
#define FSET_IWDG_KEY_WRITE  0x00005555UL /* Разрешение записи PR/RLR */
#define FSET_IWDG_KEY_RELOAD 0x0000AAAAUL /* Перезарядка счётчика     */

/* Ключи разблокировки flash (RM0383) */
#define FSET_FLASH_KEY1      0x45670123UL
#define FSET_FLASH_KEY2      0xCDEF89ABUL

/* PSIZE = 10b: программирование словами x32 (самый быстрый вариант) */
#define FSET_FLASH_PSIZE_X32 (2UL << FLASH_CR_PSIZE_Pos)

/* Биты ошибок flash-операций, при которых запись считается несостоявшейся */
#define FSET_FLASH_ERR_MASK  (FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                              FLASH_SR_PGPERR | FLASH_SR_PGSERR)

/* Заголовок записи (16 байт, выравнивание 4 — программирование x32) */
typedef struct {
    uint32_t magic;    /**< Метка валидной записи (FSET_MAGIC)          */
    uint32_t version;  /**< Версия формата (FSET_VERSION)               */
    uint32_t seq;      /**< Монотонный счётчик сохранений (выбор слоя)  */
    uint32_t crc32;    /**< CRC-32 поля data                            */
} FSet_Hdr_t;

/* Полезная нагрузка: поля InverterSettings (config.h) + уставка тока.
 * v3 (02.09.2026): +4 флага включения функций (payload 12 -> 16 байт).
 * v3.1 (02.09.2026): +operating_mode (payload 16 -> 18 байт). */
typedef struct {
    uint16_t hot_start_time_ms;    /**< Длительность горячего старта, мс */
    uint16_t hot_start_current_p;  /**< Наброс тока горячего старта, %   */
    uint16_t arc_force_time_ms;    /**< Фильтр возврата из Arc Force, мс */
    uint16_t arc_force_current_p;  /**< Наброс тока форсажа, %           */
    uint16_t anti_stick_time_ms;   /**< Время ухода в сон при КЗ, мс     */
    uint16_t weld_current_a;       /**< Уставка сварочного тока, А (v2)  */
    uint8_t  hot_start_en;         /**< 1 — Hot Start включён            */
    uint8_t  arc_force_en;         /**< 1 — Arc Force включён            */
    uint8_t  anti_stick_en;        /**< 1 — Anti-Stick включён           */
    uint8_t  vrd_en;               /**< 1 — VRD включён (OCV 24 В)       */
    uint8_t  operating_mode;       /**< Режим работы (Operating_Mode_t)  */
    uint8_t  reserved;             /**< Выравнивание до 18 байт          */
} FSet_Data_t;

typedef struct {
    FSet_Hdr_t  hdr;   /**< 16 байт */
    FSet_Data_t data;  /**< 18 байт (v3.1) */
} FSet_Record_t;       /**< 36 байт (с выравниванием компилятора до 4 байт) */

/* Раскладка структуры зафиксирована: она важна и для flash (x32-запись), и для CRC */
_Static_assert(sizeof(FSet_Hdr_t) == 16U, "FSet_Hdr_t layout broken");
_Static_assert(sizeof(FSet_Data_t) == 18U, "FSet_Data_t layout broken");
_Static_assert(sizeof(FSet_Record_t) == 36U, "FSet_Record_t layout broken");
_Static_assert((sizeof(FSet_Record_t) % 4U) == 0U, "FSet_Record_t not x32 aligned");
/* Flash программируется словами x32 (4 байта): 36 байт = 9 слов */
#define FSET_RECORD_SIZE_PROGRAM  36U

/* Флаг «настройки восстановлены из flash» (для диагностики/будущих экранов) */
static uint8_t fset_have_valid_slot = 0U;

/* Восстановленная уставка сварочного тока (А); до первого валидного слота —
 * дефолт из config.h. Применяется в main.c через Encoder_Set_Value(). */
static uint16_t fset_weld_current_a = WELD_CURRENT_DEFAULT;

/* ========================================================================== */
/*                    СЛУЖЕБНЫЕ ФУНКЦИИ (исполняются из flash)                */
/* ========================================================================== */

/**
 * @brief Программный CRC-32 (отражённый IEEE-802.3, poly 0xEDB88320).
 * @note  Аппаратный CRC в .ioc не включен; для 12 байт цикл по битам дешевле
 *        таблицы в flash. Формат совместим со стандартным CRC-32 (zlib).
 */
static uint32_t FSet_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;

    while (len-- > 0UL)
    {
        uint8_t b = *data++;
        crc ^= (uint32_t)b;
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            crc = ((crc & 1UL) != 0UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* Кламп значения в границы меню (единый источник границ — config.h, §5) */
static uint16_t FSet_Clamp_U16(uint16_t v, uint16_t lo, uint16_t hi)
{
    return (uint16_t)((v < lo) ? lo : ((v > hi) ? hi : v));
}

/**
 * @brief Проверка слота: magic + version + CRC32 над payload.
 * @return Указатель на запись или NULL (слот пуст/повреждён).
 */
static const FSet_Record_t *FSet_Slot_Validate(uint32_t slot_addr)
{
    const FSet_Record_t *rec = (const FSet_Record_t *)slot_addr;

    /* Пустой/стёртый слот: flash читается как 0xFF — magic не совпадёт */
    if (rec->hdr.magic != FSET_MAGIC)     { return NULL; }
    if (rec->hdr.version != FSET_VERSION) { return NULL; }

    if (FSet_Crc32((const uint8_t *)&rec->data, sizeof(FSet_Data_t)) != rec->hdr.crc32)
    {
        return NULL; /* Битый CRC — слот не используется */
    }
    return rec;
}

/**
 * @brief Выбор «новейшей» валидной записи (старший seq).
 * @return NULL, если оба слота невалидны.
 */
static const FSet_Record_t *FSet_Find_Newest(void)
{
    const FSet_Record_t *a = FSet_Slot_Validate(FSET_SLOT_A_ADDR);
    const FSet_Record_t *b = FSet_Slot_Validate(FSET_SLOT_B_ADDR);

    if ((a != NULL) && (b != NULL))
    {
        return (b->hdr.seq >= a->hdr.seq) ? b : a;
    }
    return (a != NULL) ? a : b;
}

/* ========================================================================== */
/*     НИЗКОУРОВНЕВЫЕ ОПЕРАЦИИ FLASH — исполняются из RAM (.RamFunc)          */
/*     Во время BSY flash-шина занята: выборка кода из flash подвисает,       */
/*     поэтому циклы ожидания и кормление IWDG должны жить в RAM. noinline    */
/*     запрещает компилятору «втянуть» их обратно в flash-функцию-оркестратор. */
/* ========================================================================== */

/**
 * @brief Ждать сброса BSY, попутно кормя IWDG (RAM).
 * @return Значение FLASH->SR после сброса BSY (для анализа ошибок).
 */
static uint32_t FSet_WaitBusy_FeedIWDG(void) __attribute__((section(".RamFunc"), noinline));
static uint32_t FSet_WaitBusy_FeedIWDG(void)
{
    uint32_t sr;

    do {
        IWDG->KR = FSET_IWDG_KEY_RELOAD; /* Кормление IWDG прямо из цикла   */
        sr = FLASH->SR;                  /* Регистр, не массив flash — доступен */
    } while ((sr & FLASH_SR_BSY) != 0UL);

    return sr;
}

/**
 * @brief Стирание целого сектора (RAM). Сектор задаётся номером для SNB.
 * @return FLASH->SR после операции (биты ошибок проверяет вызывающий).
 */
static uint32_t FSet_EraseSector_Ram(uint32_t sector_num) __attribute__((section(".RamFunc"), noinline));
static uint32_t FSet_EraseSector_Ram(uint32_t sector_num)
{
    uint32_t cr = FLASH->CR;

    cr &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);
    cr |= FLASH_CR_SER | FSET_FLASH_PSIZE_X32 |
          ((sector_num & 0x1FUL) << FLASH_CR_SNB_Pos);
    FLASH->CR = cr;
    FLASH->CR |= FLASH_CR_STRT;      /* Старт стирания (~1-2 с на 128 КБ) */

    uint32_t sr = FSet_WaitBusy_FeedIWDG();

    FLASH->CR &= ~FLASH_CR_SER;      /* Снять SER перед последующими операциями */
    return sr;
}

/**
 * @brief Программирование слов x32 в flash (RAM).
 * @return FLASH->SR после последней операции (биты ошибок проверяет вызывающий).
 */
static uint32_t FSet_ProgramWords_Ram(uint32_t dst_addr, const uint32_t *src,
                                      uint32_t word_count) __attribute__((section(".RamFunc"), noinline));
static uint32_t FSet_ProgramWords_Ram(uint32_t dst_addr, const uint32_t *src,
                                      uint32_t word_count)
{
    uint32_t sr = 0UL;
    uint32_t cr = FLASH->CR;

    cr &= ~(FLASH_CR_PSIZE | FLASH_CR_SER | FLASH_CR_SNB);
    cr |= FLASH_CR_PG | FSET_FLASH_PSIZE_X32;
    FLASH->CR = cr;

    for (uint32_t i = 0UL; i < word_count; i++)
    {
        *(__IO uint32_t *)dst_addr = src[i];   /* x32-программирование слова */
        sr = FSet_WaitBusy_FeedIWDG();
        if ((sr & FSET_FLASH_ERR_MASK) != 0UL)
        {
            break; /* Ошибка программирования — статус проверит вызывающий */
        }
        dst_addr += 4UL;
    }

    FLASH->CR &= ~FLASH_CR_PG;
    return sr;
}

/**
 * @brief Расширить окно IWDG до максимума (~4.1 с @1 кГц) на время стирания.
 * @note  Штатное окно IWDG_RELOAD_TICKS (~100 мс) меньше времени стирания
 *        сектора: суперцикл (кормление) заморожен на ~1-2 с. После операции
 *        окно возвращает FSet_IWDG_Restore_Window().
 */
static void FSet_IWDG_Widen_Window(void)
{
    IWDG->KR  = FSET_IWDG_KEY_WRITE;   /* Разрешить запись PR/RLR */
    IWDG->RLR = 0x00000FFFUL;          /* Максимальный перезаряд  */
    while ((IWDG->SR & IWDG_SR_RVU) != 0UL)
    {
        /* Ожидание синхронизации RLR в LSI-домен (RM0383) */
    }
    IWDG->KR = FSET_IWDG_KEY_RELOAD;   /* Перезарядка уже широким окном */
}

/**
 * @brief Вернуть окно IWDG к штатному (IWDG_RELOAD_TICKS) и прокормить.
 */
static void FSet_IWDG_Restore_Window(void)
{
    IWDG->KR  = FSET_IWDG_KEY_WRITE;
    IWDG->RLR = (uint32_t)IWDG_RELOAD_TICKS;
    while ((IWDG->SR & IWDG_SR_RVU) != 0UL)
    {
        /* Ожидание синхронизации RLR в LSI-домен (RM0383) */
    }
    IWDG->KR = FSET_IWDG_KEY_RELOAD;
}

/* ========================================================================== */
/*                         ПУБЛИЧНЫЙ ИНТЕРФЕЙС                                */
/* ========================================================================== */

uint8_t Flash_Settings_Have_Valid_Slot(void)
{
    return fset_have_valid_slot;
}

uint16_t Flash_Settings_Get_Weld_Current(void)
{
    return fset_weld_current_a;
}

void Flash_Settings_Restore(void)
{
    const FSet_Record_t *best = FSet_Find_Newest();

    fset_have_valid_slot = (best != NULL) ? 1U : 0U;
    if (best == NULL)
    {
        return; /* Ни одного валидного слота — дефолты config.c остаются */
    }

    /* Кламп при восстановлении: «никаких сырых чисел из flash в FSM» (§5).
     * Дублирует кламп чтения в mma_fsm.c — защита в глубину. */
    InverterSettings.hot_start_time_ms   = FSet_Clamp_U16(best->data.hot_start_time_ms,
                                                          MMA_HS_TIME_MS_MIN, MMA_HS_TIME_MS_MAX);
    InverterSettings.hot_start_current_p = FSet_Clamp_U16(best->data.hot_start_current_p,
                                                          MMA_HS_CURRENT_P_MIN, MMA_HS_CURRENT_P_MAX);
    InverterSettings.arc_force_time_ms   = FSet_Clamp_U16(best->data.arc_force_time_ms,
                                                          MMA_AF_TIME_MS_MIN, MMA_AF_TIME_MS_MAX);
    InverterSettings.arc_force_current_p = FSet_Clamp_U16(best->data.arc_force_current_p,
                                                          MMA_AF_CURRENT_P_MIN, MMA_AF_CURRENT_P_MAX);
    InverterSettings.anti_stick_time_ms  = FSet_Clamp_U16(best->data.anti_stick_time_ms,
                                                          MMA_AS_TIME_MS_MIN, MMA_AS_TIME_MS_MAX);

    /* Уставка сварочного тока (v2): кламп в физические пределы инвертора */
    fset_weld_current_a = FSet_Clamp_U16(best->data.weld_current_a,
                                         WELD_CURRENT_MIN, WELD_CURRENT_MAX);

    /* Флаги включения функций (v3): бинарные — кламп в 0/1 */
    InverterSettings.hot_start_en  = (best->data.hot_start_en  != 0U) ? 1U : 0U;
    InverterSettings.arc_force_en  = (best->data.arc_force_en  != 0U) ? 1U : 0U;
    InverterSettings.anti_stick_en = (best->data.anti_stick_en != 0U) ? 1U : 0U;
    InverterSettings.vrd_en        = (best->data.vrd_en        != 0U) ? 1U : 0U;
    /* Режим работы (v3.1): кламп в допустимые значения 0..2 */
    InverterSettings.operating_mode = (best->data.operating_mode <= (uint8_t)OPERATING_MODE_BOTH)
                                    ? best->data.operating_mode
                                    : (uint8_t)OPERATING_MODE_BOTH;
}

Flash_Settings_Status_t Flash_Settings_Save(void)
{
    const FSet_Record_t *newest = FSet_Find_Newest();
    FSet_Record_t rec;
    uint32_t tgt_addr;
    uint32_t tgt_sector;
    uint32_t primask;
    Flash_Settings_Status_t status = FLASH_SETTINGS_OK;

    /* 1. Сохранение ТОЛЬКО из STANDBY без активной аварии (§6, правило 1) */
    if ((Inverter.state != SYS_STATE_STANDBY) || (UCC3895_Is_Fault_Active() != 0U))
    {
        return FLASH_SETTINGS_ERR_STATE;
    }

    /* 2. Формируем запись: значения клампуются повторно — в flash не должно
     *    попасть «сырое» число, даже если настройки правились напрямую. */
    rec.hdr.magic   = FSET_MAGIC;
    rec.hdr.version = FSET_VERSION;
    rec.hdr.seq     = ((newest != NULL) ? newest->hdr.seq : 0UL) + 1UL;
    rec.data.hot_start_time_ms   = FSet_Clamp_U16(InverterSettings.hot_start_time_ms,
                                                  MMA_HS_TIME_MS_MIN, MMA_HS_TIME_MS_MAX);
    rec.data.hot_start_current_p = FSet_Clamp_U16(InverterSettings.hot_start_current_p,
                                                  MMA_HS_CURRENT_P_MIN, MMA_HS_CURRENT_P_MAX);
    rec.data.arc_force_time_ms   = FSet_Clamp_U16(InverterSettings.arc_force_time_ms,
                                                  MMA_AF_TIME_MS_MIN, MMA_AF_TIME_MS_MAX);
    rec.data.arc_force_current_p = FSet_Clamp_U16(InverterSettings.arc_force_current_p,
                                                  MMA_AF_CURRENT_P_MIN, MMA_AF_CURRENT_P_MAX);
    rec.data.anti_stick_time_ms  = FSet_Clamp_U16(InverterSettings.anti_stick_time_ms,
                                                  MMA_AS_TIME_MS_MIN, MMA_AS_TIME_MS_MAX);
    /* Уставка сварочного тока (v2): текущее значение Inverter.target_current
     * уходит во flash вместе с настройками меню (кламп — от «сырых» чисел) */
    rec.data.weld_current_a      = FSet_Clamp_U16((uint16_t)Inverter.target_current,
                                                  WELD_CURRENT_MIN, WELD_CURRENT_MAX);
    /* Флаги включения функций (v3): бинарные — кламп в 0/1 */
    rec.data.hot_start_en  = (InverterSettings.hot_start_en  != 0U) ? 1U : 0U;
    rec.data.arc_force_en  = (InverterSettings.arc_force_en  != 0U) ? 1U : 0U;
    rec.data.anti_stick_en = (InverterSettings.anti_stick_en != 0U) ? 1U : 0U;
    rec.data.vrd_en        = (InverterSettings.vrd_en        != 0U) ? 1U : 0U;
    /* Режим работы (v3.1): кламп в допустимые значения 0..2 */
    rec.data.operating_mode = (InverterSettings.operating_mode <= (uint8_t)OPERATING_MODE_BOTH)
                            ? InverterSettings.operating_mode
                            : (uint8_t)OPERATING_MODE_BOTH;
    rec.data.reserved = 0U;
    rec.hdr.crc32 = FSet_Crc32((const uint8_t *)&rec.data, sizeof(FSet_Data_t));

    /* 3. Цель записи — слот, ПРОТИВОПОЛОЖНЫЙ последней валидной записи:
     *    до полного завершения новой записи старая остаётся резервом
     *    (защита от пропадания питания посреди стирания/записи). */
    if (newest == NULL)
    {
        tgt_addr   = FSET_SLOT_A_ADDR;   /* Оба пусты — начинаем со слота A */
        tgt_sector = FSET_SECTOR_A_NUM;
    }
    else if (newest == (const FSet_Record_t *)FSET_SLOT_A_ADDR)
    {
        tgt_addr   = FSET_SLOT_B_ADDR;
        tgt_sector = FSET_SECTOR_B_NUM;
    }
    else
    {
        tgt_addr   = FSET_SLOT_A_ADDR;
        tgt_sector = FSET_SECTOR_A_NUM;
    }

    /* 4. Гасим мост и OCV-задание: физически закрытый выход на всё время
     *    стирания/записи (§6, правило 1). */
    UCC3895_Disable();
    UCC3895_Set_Control_Voltage(0.0f);
    PI_Regulator_Reset();

    /* 5. Критическая секция: с IRQ=0 ISR TIM2 (STANDBY-ветка) не вернёт
     *    UCC3895_Enable() и силовая математика гарантированно заморожена.
     *    SysTick/ms_tick тоже заморожены — UI-тайминги просто подождут. */
    primask = __get_PRIMASK();
    __disable_irq();

    /* 6. IWDG: штатное окно ~100 мс меньше стирания — расширяем до максимума */
    FSet_IWDG_Widen_Window();

    /* 7. Разблокировка, стирание ЦЕЛОГО сектора, программирование слов */
    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL)
    {
        FLASH->KEYR = FSET_FLASH_KEY1;
        FLASH->KEYR = FSET_FLASH_KEY2;
    }

    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL)
    {
        status = FLASH_SETTINGS_ERR_WRITE; /* Ключи не приняты */
    }
    else
    {
        uint32_t sr;

        FLASH->SR = FLASH_SR_EOP | FSET_FLASH_ERR_MASK; /* W1C: гасим старые ошибки */

        sr = FSet_EraseSector_Ram(tgt_sector);
        if ((sr & FSET_FLASH_ERR_MASK) == 0UL)
        {
            sr = FSet_ProgramWords_Ram(tgt_addr, (const uint32_t *)&rec,
                                       (uint32_t)(FSET_RECORD_SIZE_PROGRAM / 4UL));
        }

        if ((sr & FSET_FLASH_ERR_MASK) != 0UL)
        {
            status = FLASH_SETTINGS_ERR_WRITE;
        }

        FLASH->CR |= FLASH_CR_LOCK; /* Обратно под замок в любом случае */
    }

    /* 8. IWDG: вернуть штатное окно ~100 мс и разрешить прерывания —
     *    суперцикл продолжит кормление, FSM в STANDBY сам вернёт мост
     *    (UCC3895_Enable) и OCV-регулятор восстановит дежурные ~70 В */
    FSet_IWDG_Restore_Window();
    __set_PRIMASK(primask);

    if (status != FLASH_SETTINGS_OK)
    {
        return status;
    }

    /* 9. Верификация чтением: полный разбор записи + сравнение полей */
    {
        const FSet_Record_t *rd = FSet_Slot_Validate(tgt_addr);

        if ((rd == NULL) ||
            (rd->hdr.seq != rec.hdr.seq) ||
            (rd->data.hot_start_time_ms   != rec.data.hot_start_time_ms) ||
            (rd->data.hot_start_current_p != rec.data.hot_start_current_p) ||
            (rd->data.arc_force_time_ms   != rec.data.arc_force_time_ms) ||
            (rd->data.arc_force_current_p != rec.data.arc_force_current_p) ||
            (rd->data.anti_stick_time_ms  != rec.data.anti_stick_time_ms) ||
            (rd->data.weld_current_a      != rec.data.weld_current_a) ||
            (rd->data.hot_start_en        != rec.data.hot_start_en) ||
            (rd->data.arc_force_en        != rec.data.arc_force_en) ||
            (rd->data.anti_stick_en       != rec.data.anti_stick_en) ||
            (rd->data.vrd_en              != rec.data.vrd_en) ||
            (rd->data.operating_mode      != rec.data.operating_mode))
        {
            status = FLASH_SETTINGS_ERR_VERIFY;
        }
    }

    return status;
}
