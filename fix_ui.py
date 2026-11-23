#!/usr/bin/env python3
with open('/home/espdev/Documents/f411_ucc3895/Core/Src/ui_manager.c', 'r') as f:
    lines = f.readlines()

# Find the section to replace (lines 350-377, 0-indexed 349-376)
# Look for the start marker
start_idx = None
end_idx = None
for i, line in enumerate(lines):
    if '//if (ui_data_snap.state == SYS_STATE_FAULT) {' in line:
        start_idx = i
    if start_idx is not None and line.strip() == '}' and end_idx is None:
        # Check if this is the closing brace after the commented block
        if i > start_idx + 20:  # Must be a substantial block
            end_idx = i
            break

if start_idx is None:
    print("Start marker not found")
    exit(1)

if end_idx is None:
    # Try to find the closing brace
    for i in range(start_idx + 1, len(lines)):
        if lines[i].strip() == '}':
            end_idx = i
            break

print(f"Found section: lines {start_idx+1} to {end_idx+1}")

# New working code
new_code = '''    if (ui_data_snap.state == SYS_STATE_FAULT) {
        // Фаза мигания: каждые 400 мс инвертируем видимость текста аварии
        if ((ms_counter / 400U) % 2U) {
            switch (ui_data_snap.fault_code) {
                case FAULT_CODE_OVERCURRENT:  UI_PrintBigStr(16, 2, "SHORT!");  break;
                case FAULT_CODE_LOW_15V:      UI_PrintBigStr(10, 2, "220V-!");  break;
                case FAULT_CODE_OVERHEAT_IGBT:
                case FAULT_CODE_OVERHEAT_DIO: UI_PrintBigStr(28, 2, "HOT!!");  break;
                case FAULT_CODE_FAN_BLOCKED:  UI_PrintBigStr(20, 2, "FAN ER");  break;
                case FAULT_CODE_SENSOR_OPEN:  UI_PrintBigStr(10, 2, "TSENSOR"); break;
                default:                      UI_PrintBigStr(28, 2, "ERROR");  break;
            }
        }
    } else {
        /* ВЫВОД КРУПНЫХ ЦИФР ТЕКУЩЕГО ТОКА С ПОДАВЛЕНИЕМ ВЕДУЩЕГО НУЛЯ */
        char cur_buff[5];
        int current_int = (int)ui_data_snap.target_current;
        
        cur_buff[0] = (current_int / 100) + '0'; 
        cur_buff[1] = ((current_int % 100) / 10) + '0';
        cur_buff[2] = (current_int % 10) + '0';
        cur_buff[3] = 'A';
        cur_buff[4] = '\\0';
        
        if (cur_buff[0] == '0') {
            UI_PrintBigStr(38, 2, &cur_buff[1]); // Центрируем строку, если ток двухзначный
        } else {
            UI_PrintBigStr(28, 2, cur_buff);
        }
    }
'''

# Replace the section
new_lines = lines[:start_idx] + [new_code] + lines[end_idx+1:]

with open('/home/espdev/Documents/f411_ucc3895/Core/Src/ui_manager.c', 'w') as f:
    f.writelines(new_lines)

print("Done")
