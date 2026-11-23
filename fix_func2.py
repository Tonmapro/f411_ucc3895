#!/usr/bin/env python3
path = '/home/espdev/Documents/f411_ucc3895/Core/Src/ui_manager.c'
with open(path, 'r') as f:
    content = f.read()

# Replace commented-out lines with their active versions
replacements = [
    ('//OLED_Clear(OLED_COLOR_BLACK);', 'OLED_Clear(OLED_COLOR_BLACK);'),
    ('//ui_data_snap.target_current = Inverter.target_current;', 'ui_data_snap.target_current = Inverter.target_current;'),
    ('//ui_data_snap.actual_voltage = Inverter.actual_voltage;', 'ui_data_snap.actual_voltage = Inverter.actual_voltage;'),
    ('//ui_data_snap.state          = Inverter.state;', 'ui_data_snap.state          = Inverter.state;'),
    ('//ui_data_snap.fault_code     = Inverter.fault_code;', 'ui_data_snap.fault_code     = Inverter.fault_code;'),
    ('/*if (OLED_GetStatus() == OLED_BUSY_DMA) {', 'if (OLED_GetStatus() == OLED_BUSY_DMA) {'),
    ('/* 1. ЗАЩИТА ШИНЫ: Если DMA сейчас занят выталкиванием старого кадра, выходим */', '/* 1. ЗАЩИТА ШИНЫ: Если DMA сейчас занят выталкиванием старого кадра, выходим */'),
]

for old, new in replacements:
    content = content.replace(old, new)

with open(path, 'w') as f:
    f.write(content)
print("Step 2 OK")
