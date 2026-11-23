# КАК СМОТРЕТЬ КОДЫ АЦП И СОСТОЯНИЕ ПРИ ОТЛАДКЕ (cortex-debug)

Стенд: BlackPill F411 + Black Magic Probe + `gdb-multiarch`.
Конфигурация: `.vscode/launch.json` → «BMP Debug», ELF `build/Debug/f411_ucc3895.elf`.

---

## 0. ГЛАВНОЕ ПРО ПИТАНИЕ 3.18 В — КОДЫ ОТ НЕГО НЕ ЗАВИСЯТ

```
code = Vpin / VREF * 4095 ,   Vpin = Vrail * R2/(R1+R2)
```

Если делитель питается **от той же шины, что и VREF** (наш случай на макете:
и делители, и VDDA от 3.18 В), то `Vrail == VREF` и напряжение **сокращается**:

```
code = 4095 * R2 / (R1 + R2)     ← зависит ТОЛЬКО от отношения резисторов
```

| Делитель | Формула | Код | Эквивалент (V15V_SCALE=20 В) | Порог 2457 |
|---|---|---|---|---|
| **3.3к / 10к** | 4095 · 10/13.3 | **3079** | ~15.0 В | **PROHODIT** ✓ |
| 12к / 12к | 4095 · 12/24 | **2048** | ~10.0 В | НЕ проходит ✗ |
| 10к / 10к | 4095 · 10/20 | 2048 | ~10.0 В | НЕ проходит ✗ |

**Вывод:** 3.18 В вместо 3.3 В ничего не ломает — важны только номиналы.
Меняется лишь АБСОЛЮТНОЕ напряжение на пине (3.18·0.752 = 2.39 В вместо 2.48 В),
а код остаётся 3079. Поэтому проверять 3.18 В «на глаз» вольтметром нужно,
а пороги в кодах — нет.

> На БОЕВОЙ плате делитель +15 В питается от шины 15 В, а VREF = 3.3 В →
> там формула другая: `code = 15/20 * 4095 = 3071`, и пороги 2457/3686 — «12 В/18 В».

---

## 1. КРИТИЧНО: «Debug»-сборка фактически собирается с -O2

Это главная причина, почему в Watch видно `<optimized out>`.

| Место | Флаг |
|---|---|
| `cmake/gcc-arm-none-eabi.cmake:47` | `CMAKE_C_FLAGS_DEBUG = "-O0 -g3"` |
| `CMakeLists.txt:93` | `target_compile_options(... -Wall -Wextra **-O2**)` |

`-O2` стоит **последним** → GCC применяет его, `-O0` перекрыт.
Итог: **-O2 + `-g3`** (символы есть, но locals оптимизированы).

**Что это значит на практике:**
- ✅ Надёжно читаются: `Inverter`, `InverterSettings`, `global_fault_flag`,
  `ms_tick`, `public_filtered_values[]` — они глобальные/`volatile`.
- ❌ Могут быть `<optimized out>`: локальные переменные внутри функций
  (`raw_temp`, `raw_v15v`, `raw_fan_curr`, `v_phys_fixed`, `p_term`…).
  Для них — breakpoint внутри функции и чтение на остановленном ядре.

**Если нужны locals:** временно уберите `-O2` из `CMakeLists.txt:93`
(останется `-O0 -g3`) и пересоберите. ⚠️ **Но:** при `-O0` ISR TIM2 20 кГц
(50 мкс) с плавающей математикой ПИ-регулятора может не укладываться в
период → завал реального времени и сброс по IWDG. Для проверки КОДОВ АЦП
это не требуется — коды живут в глобальном массиве.

---

## 2. WATCH — готовый список для копирования

### 2.1 Состояние автомата и авария (главное на экране)

```
Inverter.state
Inverter.fault_code
global_fault_flag
Inverter.target_current
Inverter.actual_current
Inverter.actual_voltage
Inverter.out_dac_value
```

### 2.2 ВСЕ ШЕСТЬ КОДОВ АЦП (то, что вы ищете)

```
public_filtered_values[0]
public_filtered_values[1]
public_filtered_values[2]
public_filtered_values[3]
public_filtered_values[4]
public_filtered_values[5]
```

Индексы (`config.h`, `Adc_Channel_Index_t`):

| Индекс | Канал | SCALE | Порог/норма |
|---|---|---|---|
| 0 | `ADC_CURRENT_IDX` — ток дуги | 200 А | ХХ: ~0 |
| 1 | `ADC_VOLTAGE_IDX` — напряжение | 100 В | ХХ ≈ 0.7·4095/0.7 = **2870** для 70 В |
| 2 | **`ADC_V15V_IDX`** — +15 В | 20 В | **норма > 2457** (иначе «220V~!») |
| 3 | `ADC_TEMP_IGBT_IDX` — NTC моста | 120 °C | **50..4000**, перегрев < 1200 |
| 4 | `ADC_TEMP_DIO_IDX` — NTC диодов | 120 °C | 50..4000 |
| 5 | `ADC_FAN_CURR_IDX` — ток кулера | 5 А | **41..3499** (иначе «NO FAN») |

### 2.3 Внутренний EMA (до публикации) и готовность цепи

```
filtered_code_values[0]
filtered_code_values[2]
dma_process_ready
measurement_valid
dma_active_start_idx
```

### 2.4 Настройки и режим

```
InverterSettings.operating_mode
InverterSettings.vrd_en
InverterSettings.hot_start_en
InverterSettings.arc_force_en
InverterSettings.anti_stick_en
InverterSettings.hot_start_time_ms
InverterSettings.hot_start_current_p
InverterSettings.arc_force_current_p
InverterSettings.anti_stick_time_ms
```
### 2.5 Статические переменные модулей (синтаксис GDB `файл::имя`)

```
'signal_processing.c'::public_filtered_values[2]
'signal_processing.c'::filtered_code_values[2]
'signal_processing.c'::dma_process_ready
'signal_processing.c'::measurement_valid
'mma_fsm.c'::state_timer_ticks
'mma_fsm.c'::short_circuit_counter
'mma_fsm.c'::arc_restore_counter
'ui_manager.c'::menu_state
'ui_manager.c'::menu_item
'ui_manager.c'::ui_dirty
'ui_manager.c'::hold_tracked
'ui_manager.c'::last_frame_ms
'regulator.c'::pid.setpoint
'regulator.c'::pid.integrator
'regulator.c'::pid.Kp
'regulator.c'::pid.Ki
```

Если GDB не находит по короткому имени — укажите путь целиком:
`'Core/Src/signal_processing.c'::public_filtered_values[2]`

> Функционально-локальные `static` (`fan_level`, `fan_start_tick`,
> `fan_fault_counter` внутри `Thermal_Control_Process`) адресуются как
> `'main.c'::Thermal_Control_Process::fan_level`. Проще поставить
> breakpoint в `Thermal_Control_Process` и смотреть в панели VARIABLES.

---

## 3. DEBUG CONSOLE — команды GDB (MI, НЕ `-exec`!)

### 3.1 Почему `-exec` НЕ работает

Debug Console в cortex-debug — это **MI-консоль** (из README расширения:
*«All requests go to gdb and the results are read back from gdb using gdb's MI
(machine interface)»*). Поэтому ввод **начинающийся с `-`** трактуется как
команда **Machine Interface**, а не как команда GDB CLI.

В MI **нет** команды `-exec` — есть только `-exec-run`, `-exec-continue`,
`-exec-step`, `-exec-next`, `-exec-interrupt`, `-exec-finish` и т.п.
Отсюда ровно ваша ошибка:

```
Undefined MI command: exec (from exec p/x Inverter.state)
```

> Префикс `-exec` — это синтаксис **другого** адаптера (ms-vscode.cpptools /
> `cppdbg`). В cortex-debug он не нужен и не работает.

### 3.2 Правильный синтаксис: `-interpreter-exec console "..."`

Чтобы выполнить команду GDB CLI через MI, оберните её:

```gdbmi
-interpreter-exec console "p/x Inverter.state"
-interpreter-exec console "p/x Inverter.fault_code"
-interpreter-exec console "p Inverter.target_current"
-interpreter-exec console "p public_filtered_values[2]"
-interpreter-exec console "p 'signal_processing.c'::filtered_code_values[2]"
-interpreter-exec console "p/x global_fault_flag"
-interpreter-exec console "p/x ms_tick"
```

Функции (могут быть заинлайнены при -O2!):

```gdbmi
-interpreter-exec console "p Get_Filtered_Code(2)"
-interpreter-exec console "p Get_Internal_Channel(2)"
-interpreter-exec console "p Signal_Processing_Is_Data_Valid()"
```

Полезные номера:

```gdbmi
-interpreter-exec console "p ADC_CHANNELS"
-interpreter-exec console "p PING_PONG_BUFF_SIZE"
-interpreter-exec console "p sizeof(adc_raw_buffer)"
-interpreter-exec console "p &adc_raw_buffer[0]"
-interpreter-exec console "x/32hx &adc_raw_buffer[0]"
```

### 3.3 Вариант без кавычек (проверить на вашей версии)

Некоторые сборки cortex-debug оборачивают ввод **без** начального `-` в
`-interpreter-exec console`. Попробуйте просто:

```
p/x Inverter.state
```

Если увидите «Undefined MI command: ...» — вернитесь к синтаксису §3.2.

### 3.4 Если ответ `<optimized out>` / «no symbol»

Функция заинлинена и выброшена линкером (`--gc-sections`). Это **НЕ ошибка**:
читайте `public_filtered_values[2]` — он и есть результат `Get_Filtered_Code(2)`.

> ⚠️ Комментарии в `config.h` устарели: там «5 каналов, 80 ячеек, 160 элементов».
> Фактически `ADC_CHANNELS = 6` → `ADC_BUFFER_SIZE = 96`,
> `PING_PONG_BUFF_SIZE = **192**` элемента `uint16_t`.
> Доверяйте `signal_processing.h` («6 * 16 * 2 = 192»).

---

## 4. ПЕРВИЧНЫЙ СЫРОЙ БУФЕР DMA (192 half-word)

Это то, что DMA пишет напрямую из `ADC1->DR` (6 рангов × 16 выборок × 2 половины).

```
-interpreter-exec console "x/32hx &adc_raw_buffer[0]"
```

Раскладка одной половины (индексы 0..95, шаг 6 = один такт сканирования):

| Смещение | Канал |
|---|---|
| `+0` | ток (PA0) |
| `+1` | напряжение (PA1) |
| `+2` | **+15 В (PA2)** |
| `+3` | T IGBT (PA3) |
| `+4` | T диодов (PA4) |
| `+5` | ток кулера (PA5) |

Признак жизни АЦП: значения 0..4095, НЕ все `0x0000` и НЕ все `0x0FFF`.

Для визуального слежения: `-interpreter-exec console "p &adc_raw_buffer[0]"` → адрес в панель
**Memory View** (`Cortex-Debug: View Memory`).

---

## 5. CORTEX PERIPHERALS (SVD) — теперь подключён в launch.json

Было: `.vscode/STM32F411.svd` лежал на диске, но `svdFile` в `launch.json`
**не был указан** → панель пустая. Исправлено.

| Периферия | Регистр | О чём говорит |
|---|---|---|
| `ADC1` | `DR` | последняя конверсия (живой код!) |
| `ADC1` | `SR` | `OVR` = 1 → переполнение → `FAULT SENSOR_OPEN` |
| `ADC1` | `CR2` | `SWSTART`, `EXTEN`, `DMA`, `ADON` |
| `DMA2` | `LISR` | `HTIF0`/`TCIF0` — половинки буфера бегут |
| `DMA2` | `S0NDTR` | счётчик 192→0 должен «крутиться» |
| `TIM2` | `CNT` | счётчик 20 кГц — «крутится» |
| `TIM4` | `CCR1` | **задание UCC3895 в тиках (0..3840)** |
| `GPIOA` | `IDR` | уровни PA0..PA5 |
| `GPIOB` | `IDR` | **PB12 = UCC_FAULT (bit 12)**, PB10 = кнопка |
---

## 6. РАСШИФРОВКА `Inverter.state` (hex из Watch)

| Значение | Состояние | Что означает |
|---|---|---|
| `0xA5` | `SYS_STATE_STANDBY` | ХХ, мост открыт, OCV-регулятор |
| `0x5A` | `SYS_STATE_HOT_START` | поджиг, наброс тока (HS %) |
| `0x3C` | `SYS_STATE_WELDING` | стабильная сварка |
| `0xC3` | `SYS_STATE_ARC_FORCE` | капля замкнула промежуток |
| `0x96` | `SYS_STATE_ANTI_STICK` | электрод прилип → «сна» |
| `0x69` | `SYS_STATE_FAULT` | авария, мост заблокирован |

`cortex-debug.variableUseNaturalFormat = false` в `settings.json` → значения
показываются в HEX, что для `state`/`fault_code` удобно.

## 7. РАСШИФРОВКА `Inverter.fault_code`

| Код | `define` | Экран | Условие в коде |
|---|---|---|---|
| 0x00 | — | **«ERROR»** | код не выставлен + `UCC3895_Is_Fault_Active()` (PB12=0) |
| 0x01 | `OVERHEAT_IGBT` | OVERHEAT | `raw_temp < 1200` |
| 0x02 | `LOW_15V` | 220V~! | `raw_v15v < 2457` (PA2) |
| 0x03 | `FAN_BLOCKED` | NO FAN | ток кулера >3500 или <40 |
| 0x04 | `OVERCURRENT` | SHORT | EXTI12 (PB12 спад) **или** FSM-барьер |
| 0x05 | `SENSOR_OPEN` | TSENSOR | `raw_temp <= 50` или `>= 4000`; OVR/TE DMA |
| 0x06 | `OVERHEAT_DIO` | (ERROR) | **нигде не выставляется** |
| 0x07 | `POWER_FAIL` | LOW VDD | PVD < 2.8 В |
| 0x08 | `CLOCK_FAIL` | CLK ERR | отказ HSE (CSS/NMI) |

## 8. ДЕРЕВО «ПОЧЕМУ АВАРИЯ» — за 4 выражения

Всё это удобно смотреть в **WATCH** (без команд) или через Debug Console с
обёрткой `-interpreter-exec console "..."` (см. §3.2):

1. `p/x Inverter.fault_code` — код.
2. `p/x global_fault_flag` — `1` → защёлка EXTI12 сработала
   (клик кнопкой теперь её сбрасывает: `UCC3895_Clear_Fault()`).
3. Через SVD: **GPIOB→IDR бит 12** = 0 → пин PB12 прижат → «SHORT»/«ERROR».
4. `p 'signal_processing.c'::public_filtered_values[2]` — если < 2457
   → это PA2/делитель, а не логика.

### Быстрая проверка порогов (WATCH / Debug Console)

```gdbmi
-interpreter-exec console "p 'signal_processing.c'::public_filtered_values[3] < 1200"
-interpreter-exec console "p 'signal_processing.c'::public_filtered_values[3] <= 50"
-interpreter-exec console "p 'signal_processing.c'::public_filtered_values[3] >= 4000"
-interpreter-exec console "p 'signal_processing.c'::public_filtered_values[2] < 2457"
-interpreter-exec console "p 'signal_processing.c'::public_filtered_values[5] > 3500"
```

> Сравнения вида `x < 1200` в **WATCH**-панели cortex-debug вычисляет сам —
> можно вписать прямо туда, без команд.

---

## 9. ВАЖНО ПРИ ОТЛАДКЕ НА ЖЕЛЕЗЕ

* IWDG (~100 мс) **заморожен** в Debug только при `-DDEBUG` — и это уже
  сделано (`main.c:224-229`, `DBGMCU->APB1FZ |= DBG_IWDG_STOP`). На
  breakpoint'ах сброса не будет.
* Чтение Watch через BMP **останавливает ядро**, поэтому TIM2 20 кГц
  пропускает тики. Это искажает FSM-тайминги (счётчики в тиках!) — по
  `'mma_fsm.c'::state_timer_ticks` судить о реальном времени нельзя.
  Для временных измерений используйте `ms_tick` (SysTick), а не тики TIM2.
* `Live Watch` (включён, 4 Гц) делает то же самое, но кратко — для
  медленных величин (`public_filtered_values`, `Inverter`) безопасно.

## 10. ШПАРГАЛКА «КОД → ФИЗИКА»

```
физика = код * SCALE / 4095
обратно: код = физика * 4095 / SCALE

Ток:        код = A  * 4095 / 200
Напряжение: код = В  * 4095 / 100   (70 В → 2866, 60 В → 2457, 19 В → 778, 10 В → 410)
+15В:       код = В  * 4095 / 20    (15 В → 3071, 12 В → 2457)
NTC:        код = °C * 4095 / 120   (NTC: горячее → код МЕНЬШЕ)
Кулер:      код = A  * 4095 / 5
```

`ADC_THRES_NO_LOAD = 2459` (~60 В) и `ADC_THRES_15V_MIN = 2457` (~12 В) —
**разные каналы с почти одинаковым числом**; не перепутайте при чтении Watch.
