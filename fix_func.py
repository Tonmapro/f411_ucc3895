#!/usr/bin/env python3
path = '/home/espdev/Documents/f411_ucc3895/Core/Src/ui_manager.c'
with open(path, 'r') as f:
    content = f.read()

# Step 1: Remove the test stub at the beginning
stub = '''void UI_Manager_Update(void) {
    OLED_Clear(OLED_COLOR_BLACK);
    UI_PrintBigStr(0, 0, "OK");
    OLED_Swap_Buffers();
    /*'''

replacement = '''void UI_Manager_Update(void) {
    ms_counter = System_Get_Tick(); // Получаем системное время (SysTick, 1 мс)

    /* 1. ЗАЩИТА ШИНЫ: Если DMA сейчас занят выталкиванием старого кадра, выходим */
    if (OLED_GetStatus() == OLED_BUSY_DMA) {
        return;
    }

    /* 2. ОЧИСТКА: Обнуляем теневой (Back) буфер ОЗУ через memset за 1 такт */
    OLED_Clear(OLED_COLOR_BLACK);
    
    /* 3. АТОМАРНЫЙ СНИМОК: Копируем данные из быстрого прерывания FSM (20 кГц) */
    ui_data_snap.target_current = Inverter.target_current;
    ui_data_snap.actual_voltage = Inverter.actual_voltage;
    ui_data_snap.state          = Inverter.state;
    ui_data_snap.fault_code     = Inverter.fault_code; 

    /*'''

if stub in content:
    content = content.replace(stub, replacement)
    with open(path, 'w') as f:
        f.write(content)
    print("Step 1 OK")
else:
    print("Stub not found")
