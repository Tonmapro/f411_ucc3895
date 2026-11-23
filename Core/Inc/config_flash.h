#ifndef INC_CONFIG_FLASH_H_
#define INC_CONFIG_FLASH_H_

#include "main.h"

#define FLASH_SETTING_SECTOR    3
#define FLASH_SETTING_ADDRESS   0x0800C000  // Начало сектора 3 (16 КБ: 0x0800C000..0x0800FFFF)
#define CONFIG_MAGIC_NUMBER     0x5A415641  // Уникальный маркер
#define CONFIG_VERSION          0x0002U     // Версия формата (защита от чужой/старой раскладки)

/* Структура, физически лежащая во Flash (12 байт, без выравнивающих дыр).
 * Храним то, что реально использует архитектура FSM: режим (MMA/Lift-TIG)
 * и уставку тока. Инженерные тайминги остаются в config.h. */
typedef struct {
    uint32_t magic;           // Маркер проверки
    uint16_t version;         // Версия формата
    uint16_t welder_mode;     // 0 - MMA, 1 - Lift-TIG
    uint16_t target_current;  // Уставка тока, Амперы
    uint16_t reserved;        // Запас под будущие поля
} FlashConfig_t;

// Прототипы функций
void Config_Load(void);           // Чтение при старте (валидация magic+version)
void Config_Save(void);           // Стирание сектора 3 + запись (только в STANDBY!)
void Config_Check_Timeout(void);  // Отложенная запись через 3 с тишины (вызов из while(1))
void Config_Request_Save(void);   // Взвод флага отложенного сохранения

#endif /* INC_CONFIG_FLASH_H_ */