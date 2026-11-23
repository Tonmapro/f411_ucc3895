cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME f411_ucc3895)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/cooling.c
    Core/Src/config.c
    Core/Src/config_flash.c   # Сохранение уставок во Flash (переведен на Inverter.*)
    Core/Src/encoder.c
    Core/Src/hwd_ucc3895.c
    Core/Src/mma_fsm.c
    Core/Src/oled_ssd1306.c
    Core/Src/regulator.c
    Core/Src/signal_processing.c
    Core/Src/ui_manager.c
)
# ИСКЛЮЧЕНЫ из сборки (старая архитектура на HAL, см. Docs/pin_map.md):
#   Core/Src/welder.c       - старая FSM на TIM1, не вызывается
#   Core/Src/display.c      - старый SSD1306-драйвер на HAL, заменён oled_ssd1306.c


# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Core/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# --- Флаги компиляции (для генерации кода) ---
add_compile_options(
    -mcpu=cortex-m4
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv4-sp-d16
)

# --- Флаги линковщика (собирают программу воедино) ---
add_link_options(
    -mcpu=cortex-m4
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv4-sp-d16
    --specs=nano.specs
    
    # ВОТ ЭТОТ ФЛАГ ОЖИВИТ СИМВОЛЫ FLOAT В SPRINTF:
    -u _printf_float
)
