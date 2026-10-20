/*****************************************************************************************************************************
 * ota-fortress Bootloader (STM32F407VET6 / Keil MDK port)
 * Secondary development based on an upstream open-source FOTA project
 * Target MCU: STM32F407VET6 (512 KB Flash, 8 sectors, 128 KB SRAM)
 *****************************************************************************************************************************/

#ifndef BOOTLOADER_H_
#define BOOTLOADER_H_

#include <stdint.h>

/* Communication peripheral used to talk with the HOST (ESP32 or PC) */
#define BL_HOST_COMMUNICATION_UART       &huart1
#define CRC_ENGINE_OBJ                   &hcrc

#define BL_HOST_BUFFER_RX_LENGTH         100

/* UART window after reset: if the HOST sends a byte within this time the
 * bootloader stays in update mode, otherwise it auto-boots the active slot. */
#define BL_AUTOBOOT_WINDOW_MS            1000U

/* IWDG (independent watchdog, LSI ~32 kHz).
 * Started at EVERY jump to the app: once enabled it can only be stopped by a
 * power-on reset, so both the app (main loop) and the bootloader (idle loop /
 * between long flash operations) must keep refreshing it.
 * Reload = 1250 @ /256 -> ~10 s typical (>= ~6.8 s with a fast LSI), which
 * always exceeds the longest single-sector flash erase (~4 s worst case). */
#define BL_IWDG_PRESCALER_VALUE          0x06U   /* /256 (IWDG_PR_PR_256) */
#define BL_IWDG_RELOAD_VALUE             1250U
#define BL_IWDG_REFRESH_KEY              0xAAAAU

/* Bounded UART waits: the bootloader refreshes the watchdog while idling, so
 * a leftover IWDG from a previous app boot never resets an idle session. */
#define BL_UART_IDLE_POLL_MS             1000U
#define BL_UART_BODY_TIMEOUT_MS          2000U

/*************************************  Bootloader Commands  *************************************/
#define CBL_GET_CID_CMD                  0x10  /* Get chip ID                                  */
#define CBL_GET_RDP_STATUS_CMD           0x11  /* Read flash protection level                  */
#define CBL_GO_TO_ADDR_CMD               0x12  /* Jump to user application                     */
#define CBL_FLASH_ERASE_CMD              0x13  /* Erase user application flash sectors         */
#define CBL_MEM_WRITE_CMD                0x14  /* Write 64-byte payload to user application    */
#define CBL_GET_SLOT_STATUS_CMD          0x15  /* Get active slot / state / image length       */
#define CBL_ERASE_SLOT_CMD               0x16  /* Erase slot A (0) or B (1)                    */
#define CBL_ACTIVATE_CMD                 0x17  /* Verify whole image CRC, set TRIAL, jump      */
#define CBL_CAN_INJECT_CMD               0x1C  /* TEST: inject one CAN frame (loopback builds) */

/*************************************  Signed Images  ******************************************/
/* 1 = production: ACTIVATE rejects any package without a valid TLV signature trailer.
 * 0 = debug build: unsigned packages activate too (bring-up only, never ship). */
#define BL_REQUIRE_SIGNED_IMAGES        1U

/* ACTIVATE reply codes (1 byte). The status byte is sent FIRST, any human
 * readable log line goes out AFTER it (the host drains it as text). */
#define BL_ACT_STATUS_SUCCESS           0x01U
#define BL_ACT_FAIL_VALIDATION          0x00U  /* slot range / image bounds / vector / CRC */
#define BL_ACT_FAIL_NO_TLV              0x10U  /* signature trailer missing or malformed   */
#define BL_ACT_FAIL_DIGEST              0x11U  /* recomputed digest != trailer digest      */
#define BL_ACT_FAIL_SIGNATURE           0x12U  /* ECDSA-P256 verification failed           */
#define BL_ACT_FAIL_SEC_VERSION         0x13U  /* security_version below min_sv floor      */

/*************************************  CRC Verification  *************************************/
#define CRC_TYPE_SIZE_BYTE               4
#define CRC_VERIFICATION_FAILED          0x00
#define CRC_VERIFICATION_PASSED          0x01

#define CBL_SEND_NACK                    0xAB

/*************************************  Memory Map (F407VE)  ***********************************/
/* Bootloader occupies sector 0 and 1 : 0x08000000 - 0x08007FFF (32 KB) */
/* Metadata occupies sector 2         : 0x08008000 - 0x0800BFFF (16 KB) */
/* Slot A (sectors 3-5)               : 0x0800C000 - 0x0803FFFF (208 KB) */
/* Slot B (sectors 6-7)               : 0x08040000 - 0x0807FFFF (256 KB) */
#define SLOT_A_BASE_ADDRESS              0x0800C000U
#define SLOT_A_END_ADDRESS               0x08040000U
#define SLOT_A_SECTOR_START              3U
#define SLOT_A_SECTOR_COUNT              3U

#define SLOT_B_BASE_ADDRESS              0x08040000U
#define SLOT_B_END_ADDRESS               0x08080000U
#define SLOT_B_SECTOR_START              6U
#define SLOT_B_SECTOR_COUNT              2U

#define ADDRESS_IS_INVALID               0x00
#define ADDRESS_IS_VALID                 0x01

#define STM32F4_SRAM1_SIZE               (128 * 1024)   /* F407VE SRAM */
#define STM32F4_FLASH_SIZE               (512 * 1024)   /* F407VE Flash */
#define STM32F4_SRAM1_END                (SRAM1_BASE + STM32F4_SRAM1_SIZE)
#define STM32F4_FLASH_END                (FLASH_BASE + STM32F4_FLASH_SIZE)

/*************************************  Flash Erase  *******************************************/
#define CBL_FLASH_MAX_SECTOR_NUMBER      8       /* F407VE has 8 sectors */
#define CBL_FLASH_MASS_ERASE             0xFF

#define INVALID_SECTOR_NUMBER            0x00
#define VALID_SECTOR_NUMBER              0x01
#define UNSUCCESSFUL_ERASE               0x02
#define SUCCESSFUL_ERASE                 0x03

#define HAL_SUCCESSFUL_ERASE             0xFFFFFFFFU

/*************************************  Memory Write  *******************************************/
#define FLASH_PAYLOAD_WRITE_FAILED       0x00
#define FLASH_PAYLOAD_WRITE_PASSED       0x01

#define FLASH_LOCK_WRITE_FAILED          0x00
#define FLASH_LOCK_WRITE_PASSED          0x01

/*************************************  ROP Level  **********************************************/
#define ROP_LEVEL_READ_INVALID           0x00
#define ROP_LEVEL_READ_VALID             0x01

#define CBL_ROP_LEVEL_0                  0x00
#define CBL_ROP_LEVEL_1                  0x01
#define CBL_ROP_LEVEL_2                  0x02

/*************************************  Data Types  *********************************************/
typedef enum
{
	BL_NACK = 0,
	BL_OK
} BL_Status;

typedef void (*pMainApp)(void);
typedef void (*Jump_Ptr)(void);

/*************************************  Interfaces  *********************************************/
void BL_Boot_Entry(void);
BL_Status BL_UART_Fetch_Host_Command(void);
BL_Status BL_UART_Process_Host_Command(uint8_t first_byte);
BL_Status BL_Process_Packet(const uint8_t *Host_Buffer);
void BL_Set_Transport(uint8_t is_can);
void Bootloader_Jump_To_Active_App(void);
void BL_IWDG_Refresh(void);
uint8_t Bootloader_CRC_Verify(uint8_t *pData, uint32_t Data_Len, uint32_t Host_CRC);
uint32_t Bootloader_CRC_Calculate(const uint8_t *pData, uint32_t Data_Len);

#endif /* BOOTLOADER_H_ */
