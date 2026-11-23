#include "stdint.h" 
typedef enum {
    WELD_SYS_STATE_INIT = 0,
    WELD_SYS_STATE_IDLE,         // Холостой ход (Ожидание)
    WELD_SYS_STATE_HOT_START,    // Наброс тока при поджиге
    WELD_SYS_STATE_WELDING,      // Стабильная сварка + Arc Force
    WELD_SYS_STATE_ANTI_STICK,   // Электрод залип (Ток в минимум)
    WELD_SYS_STATE_FAULT,        // Авария (Перегрев, КЗ)
    WELD_SYS_STATE_ARC_FORCE,     // Увеличивает уставку тока
    WELD_SYS_STATE_STANDBY
} WeldState_t;

extern volatile WeldState_t current_state;