#ifndef SERVICES_INC_FLASH_SETTINGS_H_
#define SERVICES_INC_FLASH_SETTINGS_H_

#include <stdint.h>

/**
 * @brief  Статус операции сохранения настроек во внутренний flash.
 */
typedef enum {
    FLASH_SETTINGS_OK = 0,      /**< Запись и контрольное чтение прошли успешно */
    FLASH_SETTINGS_ERR_STATE,   /**< Разрешено только из STANDBY без активной аварии */
    FLASH_SETTINGS_ERR_WRITE,   /**< Ошибка разблокировки/стирания/программирования */
    FLASH_SETTINGS_ERR_VERIFY   /**< Контрольное чтение не совпало с записанным */
} Flash_Settings_Status_t;

/**
 * @brief  Восстановление инженерных настроек из flash в InverterSettings.
 * @note   Сканирует слоты A/B (сектора 6/7 @ 0x0804xxxx/0x0806xxxx), выбирает
 *         валидный (magic+version+CRC32) с наибольшим seq, клампует значения
 *         в границы меню (config.h) и записывает в InverterSettings. Если ни
 *         один слот не валиден — дефолты config.c остаются нетронутыми.
 *         Только чтение: безопасно вызывать ДО инициализации периферии и IWDG
 *         (main.c вызывает перед UI_Manager_Init, план §7 п.3).
 */
void Flash_Settings_Restore(void);

/**
 * @brief  Сохранение текущих InverterSettings во flash (слот, противоположный
 *         последней валидной записи; старый слот остаётся резервом).
 * @note   РАЗРЕШЕНО ТОЛЬКО из SYS_STATE_STANDBY без активной аварии (§6):
 *         перед стиранием мост гасится (UCC3895_Disable + 0 В), прерывания
 *         запрещаются, окно IWDG временно расширяется до ~4.1 с, стирание/
 *         программирование ждут BSY из RAM (.RamFunc). Блокирует суперцикл
 *         на время стирания сектора (~1-2 с).
 * @return FLASH_SETTINGS_OK при успехе, иначе код ошибки.
 */
Flash_Settings_Status_t Flash_Settings_Save(void);

/**
 * @brief  Была ли валидная запись настроек обнаружена при последнем Restore.
 * @return 1 — настройки загружены из flash, 0 — работают дефолты config.c
 */
uint8_t Flash_Settings_Have_Valid_Slot(void);

/**
 * @brief  Восстановленная уставка сварочного тока (формат v2, 01.09.2026).
 * @note   Возвращает клампированное значение из валидного flash-слота либо
 *         WELD_CURRENT_DEFAULT (config.h), если валидных записей нет.
 *         Значение применяется в main.c через Encoder_Set_Value() — и в
 *         Inverter.target_current, и как база для отсчёта шагов энкодера.
 * @return Уставка сварочного тока в Амперах (WELD_CURRENT_MIN..MAX)
 */
uint16_t Flash_Settings_Get_Weld_Current(void);

#endif /* SERVICES_INC_FLASH_SETTINGS_H_ */
