/**
 ******************************************************************************
 * @file    main.h
 * @brief   Header for main.c - FOTA user application (STM32F407VET6)
 *          Built twice (slot A at 0x0800C000, slot B at 0x08040000) and
 *          jumped to by the dual-slot FOTA bootloader at 0x08000000.
 ******************************************************************************
 */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* Application entry address (must match the linker IROM1 start address).
 * Build the slot B image with the preprocessor define APP_SLOT=1. */
#if defined(APP_SLOT) && (APP_SLOT == 1)
#define APP_BASE_ADDRESS                 0x08040000U
#define APP_SLOT_ID                      1U
#else
#define APP_BASE_ADDRESS                 0x0800C000U
#define APP_SLOT_ID                      0U
#endif

/* Demo LED (PE9 on the user's F407 board - change to match your hardware) */
#define APP_LED_GPIO_PORT                GPIOE
#define APP_LED_GPIO_PIN                 GPIO_PIN_9
#define APP_LED_GPIO_CLK_ENABLE()        __HAL_RCC_GPIOE_CLK_ENABLE()

/* Heartbeat message period in ms */
#define APP_HEARTBEAT_PERIOD_MS          1000U
#define APP_LED_TOGGLE_PERIOD_MS         500U

/* After running healthy for this long, confirm the update in the metadata
 * sector (TRIAL -> IDLE). An unconfirmed slot is rolled back on next boot. */
#define APP_CONFIRM_DELAY_MS             3000U

/* UART magic byte: resets the MCU into the bootloader (auto-boot window). */
#define APP_REBOOT_MAGIC                 0xAAU

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
