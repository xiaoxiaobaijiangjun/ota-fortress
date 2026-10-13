/*****************************************************************************************************************************
 * FOTA-STM32 Bootloader (STM32F407VET6 / Keil MDK port)
 * Based on: https://github.com/EhabMagdyy/FOTA-STM32
 *****************************************************************************************************************************/

/************************************             Includes               ************************************/
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "bootloader.h"
#include "fota_meta.h"
#include "usart.h"
#include "crc.h"
#include "sha256.h"
#include "image_tlv.h"
#include "uECC.h"
#include "bootloader_key.h"
#include "can_transport.h"

/************************************    Static Functions Decelerations  ************************************/
static void Bootloader_Get_Chip_Identification_Number(uint8_t *Host_Buffer);
static void Bootloader_Read_Protection_Level(uint8_t *Host_Buffer);
static void Bootloader_Jump_To_User_App(uint8_t *Host_Buffer);
static void Bootloader_Erase_Flash(uint8_t *Host_Buffer);
static void Bootloader_Memory_Write(uint8_t *Host_Buffer);
static void Bootloader_Get_Slot_Status(uint8_t *Host_Buffer);
static void Bootloader_Erase_Slot(uint8_t *Host_Buffer);
static void Bootloader_Activate(uint8_t *Host_Buffer);

uint8_t Bootloader_CRC_Verify(uint8_t *pData, uint32_t Data_Len, uint32_t Host_CRC);
uint32_t Bootloader_CRC_Calculate(const uint8_t *pData, uint32_t Data_Len);
static void Bootloader_Send_NACK(void);
static void Bootloader_Send_Data_To_Host(uint8_t *Host_Buffer, uint32_t Data_Len);
static void BL_Send_Raw(const uint8_t *pData, uint32_t Data_Len);
static uint8_t Host_Address_Verification(uint32_t Jump_Address, uint16_t Payload_Len);
static uint8_t Perform_Flash_Erase(uint8_t Sector_Numebr, uint8_t Number_Of_Sectors);

/* A/B slot management */
static uint8_t Meta_Read(FOTA_Meta_t *meta);
static uint8_t Meta_Write(const FOTA_Meta_t *meta);
static uint8_t Bootloader_Slot_Range(uint8_t slot, uint32_t *base, uint32_t *end);
static uint8_t Bootloader_Is_App_Valid(uint32_t base, uint32_t end);
static uint8_t Bootloader_App_Trusted(uint8_t slot, uint32_t base, uint32_t end);
static void Bootloader_Jump_To_App_At(uint32_t base);
static uint8_t Bootloader_Resolve_Boot_Slot(void);
static uint8_t Bootloader_Image_CRC_Verify(uint32_t base_addr, uint32_t image_len, uint32_t expected);
static void BL_IWDG_Start(void);
void BL_IWDG_Refresh(void);
#if BL_REQUIRE_SIGNED_IMAGES
static uint8_t Bootloader_Verify_Signed_Image(uint32_t base, uint32_t image_len, uint32_t slot_end,
                                               uint32_t *sv_out, uint32_t *verify_ms_out);
#endif
static void BL_Log(const char *fmt, ...);
static const char *BL_Act_Reason(uint8_t status);

/************************************    Global Variables Definitions     ************************************/
static uint8_t BL_Host_Buffer[BL_HOST_BUFFER_RX_LENGTH];

/* Which carrier carries the CURRENT command's replies. Set by the transport
 * that received the packet (UART / CAN) before dispatch; "swap the physical
 * layer, keep the protocol layer". */
static uint8_t BL_Cmd_Transport_Is_CAN = 0U;

void BL_Set_Transport(uint8_t is_can)
{
	BL_Cmd_Transport_Is_CAN = is_can;
}

static void BL_Send_Raw(const uint8_t *pData, uint32_t Data_Len)
{
	if(BL_Cmd_Transport_Is_CAN)
	{
		(void)BL_CAN_Send_Packet(pData, (uint16_t)Data_Len);
	}
	else
	{
		(void)HAL_UART_Transmit(BL_HOST_COMMUNICATION_UART, (uint8_t *)pData, Data_Len, HAL_MAX_DELAY);
	}
}

/************************************ Software Interfaces Implementations ************************************/
void BL_Boot_Entry(void)
{
	uint8_t first_byte = 0;
	uint32_t window_start;

	/* A previous app boot may have left the IWDG running: keep it fed. */
	BL_IWDG_Refresh();

	/* Bring up the CAN transport (loopback in C1, real bus in C2). */
	BL_CAN_Init();

	/* Small window after reset: if the HOST sends a UART byte we stay in
	 * update mode on the UART; if CAN frames arrive we serve them; otherwise
	 * we auto-boot the active application slot. CAN activity extends the
	 * window so a full CAN session never times out mid-update. */
	window_start = HAL_GetTick();
	while((HAL_GetTick() - window_start) < BL_AUTOBOOT_WINDOW_MS)
	{
		BL_IWDG_Refresh();
		if(HAL_UART_Receive(BL_HOST_COMMUNICATION_UART, &first_byte, 1, 10) == HAL_OK)
		{
			(void)BL_UART_Process_Host_Command(first_byte);
			while (1)
			{
				BL_UART_Fetch_Host_Command();
			}
		}
		if(BL_CAN_Activity())
		{
			BL_CAN_Service();
			window_start = HAL_GetTick();
		}
	}

	BL_IWDG_Refresh();
	/* No HOST activity: boot the active slot (unconfirmed TRIAL rolls back). */
	Bootloader_Jump_To_Active_App();

	/* No valid application found anywhere: wait for the HOST. */
	while (1)
	{
		BL_UART_Fetch_Host_Command();
	}
}

BL_Status BL_UART_Fetch_Host_Command(void)
{
	uint8_t first_byte = 0;
	HAL_StatusTypeDef HAL_Status = HAL_TIMEOUT;

	/* Wait for the length byte of the next command packet, refreshing the
	 * IWDG on every idle poll so an idle bootloader session never resets. */
	do
	{
		HAL_Status = HAL_UART_Receive(BL_HOST_COMMUNICATION_UART, &first_byte, 1, BL_UART_IDLE_POLL_MS);
		if(HAL_Status == HAL_TIMEOUT)
		{
			BL_IWDG_Refresh();
		}
	} while(HAL_Status == HAL_TIMEOUT);

	if(HAL_Status != HAL_OK)
	{
		return BL_NACK;
	}
	return BL_UART_Process_Host_Command(first_byte);
}

BL_Status BL_UART_Process_Host_Command(uint8_t first_byte)
{
	BL_Status Status = BL_NACK;
	HAL_StatusTypeDef HAL_Status = HAL_OK;
	uint8_t Data_Length = 0;

	memset(BL_Host_Buffer, 0, BL_HOST_BUFFER_RX_LENGTH);

	BL_Host_Buffer[0] = first_byte;
	Data_Length = BL_Host_Buffer[0];
	if(Data_Length == 0)
	{
		Status = BL_NACK;
		Bootloader_Send_NACK();
	}
	else if(Data_Length > (BL_HOST_BUFFER_RX_LENGTH - 1))
	{
		Status = BL_NACK;
		Bootloader_Send_NACK();
	}
	else
	{
		/* Read the command packet received from the HOST (bounded: the host
		 * always sends a full packet, a stalled one must not trip the IWDG) */
		HAL_Status = HAL_UART_Receive(BL_HOST_COMMUNICATION_UART, &BL_Host_Buffer[1], Data_Length, BL_UART_BODY_TIMEOUT_MS);

		if(HAL_Status != HAL_OK)
		{
			Status = BL_NACK;
		}
		else
		{
			Status = BL_Process_Packet(BL_Host_Buffer);
		}
	}

	return Status;
}

/* Protocol layer, carrier agnostic: the packet [len][cmd][payload][crc32] is
 * fully reassembled in memory by whichever transport delivered it (UART body
 * read above, CAN ISO-TP reassembly in can_transport.c). All handlers and
 * the upgrade state machine live below this line, unchanged for both. */
BL_Status BL_Process_Packet(const uint8_t *Host_Buffer)
{
	BL_Status Status = BL_NACK;

	switch(Host_Buffer[1]){
		case CBL_GET_CID_CMD:
			Bootloader_Get_Chip_Identification_Number((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_GET_RDP_STATUS_CMD:
			Bootloader_Read_Protection_Level((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_GO_TO_ADDR_CMD:
			Bootloader_Jump_To_User_App((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_FLASH_ERASE_CMD:
			Bootloader_Erase_Flash((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_MEM_WRITE_CMD:
			Bootloader_Memory_Write((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_GET_SLOT_STATUS_CMD:
			Bootloader_Get_Slot_Status((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_ERASE_SLOT_CMD:
			Bootloader_Erase_Slot((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
		case CBL_ACTIVATE_CMD:
			Bootloader_Activate((uint8_t *)Host_Buffer);
			Status = BL_OK;
			break;
#if BL_CAN_LOOPBACK_TEST
		case CBL_CAN_INJECT_CMD:
			/* Test harness: put one CAN frame on the (looping-back) bus and
			 * service it synchronously. Replies come back as forwarded CAN
			 * frames on the UART - this command itself never answers. */
			(void)BL_CAN_Inject_And_Service(&((uint8_t *)Host_Buffer)[2]);
			Status = BL_OK;
			break;
#endif
		default:
			/* Unknown command: reject the packet */
			Bootloader_Send_NACK();
			Status = BL_NACK;
			break;
	}

	return Status;
}

/************************************    Static Functions Implementations  ************************************/
static void Bootloader_Get_Chip_Identification_Number(uint8_t *Host_Buffer)
{
	uint16_t Host_CMD_Packet_Len = 0;
	uint32_t Host_CRC32 = 0;
	uint16_t MCU_Identification_Number = 0;
	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32)){
		/* Get the MCU chip identification number */
		MCU_Identification_Number = (uint16_t)((DBGMCU->IDCODE) & 0x00000FFF);
		/* Report chip identification number to HOST */
		Bootloader_Send_Data_To_Host((uint8_t *)&MCU_Identification_Number, 2);
	}
	else{
		Bootloader_Send_NACK();
	}
}

static uint8_t CBL_STM32F4_Get_RDP_Level(void)
{
	FLASH_OBProgramInitTypeDef FLASH_OBProgram;
	/* Get the Option byte configuration */
	HAL_FLASHEx_OBGetConfig(&FLASH_OBProgram);

	return (uint8_t)(FLASH_OBProgram.RDPLevel);
}

static void Bootloader_Read_Protection_Level(uint8_t *Host_Buffer)
{
	uint16_t Host_CMD_Packet_Len = 0;
	uint32_t Host_CRC32 = 0;
	uint8_t RDP_Level = 0;

	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32)){
		/* Read Protection Level */
		RDP_Level = CBL_STM32F4_Get_RDP_Level();
		if(0xAA == RDP_Level)		 RDP_Level = 0x00;
		else if(0x55 == RDP_Level)   RDP_Level = 0x01;
		/* Report Valid Protection Level */
		Bootloader_Send_Data_To_Host((uint8_t *)&RDP_Level, 1);
	}
	else{
		Bootloader_Send_NACK();
	}
}

static void Bootloader_Jump_To_User_App(uint8_t *Host_Buffer)
{
	uint16_t Host_CMD_Packet_Len = 0;
	uint32_t Host_CRC32 = 0;
	uint8_t appExists = 0;

	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32))
	{
		uint8_t slot = Bootloader_Resolve_Boot_Slot();
		uint32_t base = 0;
		uint32_t end = 0;

		/* A valid application must start with a sane stack pointer and a
		 * reset handler inside its own slot (plus whole-image CRC when the
		 * metadata recorded one) */
		if(Bootloader_Slot_Range(slot, &base, &end) && Bootloader_App_Trusted(slot, base, end))
		{
			appExists = 1;
			Bootloader_Send_Data_To_Host((uint8_t *)&appExists, 1);
			Bootloader_Jump_To_App_At(base);
			return;
		}

		/* Active slot invalid -> try the other slot */
		slot = (slot == META_SLOT_A) ? META_SLOT_B : META_SLOT_A;
		if(Bootloader_Slot_Range(slot, &base, &end) && Bootloader_App_Trusted(slot, base, end))
		{
			appExists = 1;
			Bootloader_Send_Data_To_Host((uint8_t *)&appExists, 1);
			Bootloader_Jump_To_App_At(base);
			return;
		}
		Bootloader_Send_Data_To_Host((uint8_t *)&appExists, 1);
	}
	else{
		Bootloader_Send_NACK();
	}
}

/************************************    A/B Slot Management    ************************************/
static uint8_t Meta_Read(FOTA_Meta_t *meta)
{
	const uint8_t *p = (const uint8_t *)META_BASE_ADDRESS;

	/* Blank sector (0xFF) on a fresh chip -> no metadata yet */
	if(p[0] == 0xFF)
	{
		return 0;
	}

	memcpy(meta, p, sizeof(FOTA_Meta_t));
	if(meta->magic != META_MAGIC)
	{
		return 0;
	}
	if(meta->version != META_VERSION)
	{
		return 0;
	}
	/* Whole-record integrity: CRC over the META_CRC_COVERED_LEN bytes before meta_crc */
	return (Bootloader_CRC_Calculate(p, META_CRC_COVERED_LEN) == meta->meta_crc);
}

static uint8_t Meta_Write(const FOTA_Meta_t *meta)
{
	FOTA_Meta_t tmp = *meta;
	FLASH_EraseInitTypeDef EraseInit = {0};
	uint32_t SectorError = 0;
	uint32_t i = 0;
	uint8_t ok = 0;

	/* Recompute the record CRC before storing */
	tmp.meta_crc = Bootloader_CRC_Calculate((const uint8_t *)&tmp, META_CRC_COVERED_LEN);

	/* Metadata lives in its own sector (2): erasing it never touches a slot */
	HAL_FLASH_Unlock();
	EraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
	EraseInit.Banks = FLASH_BANK_1;
	EraseInit.Sector = META_SECTOR_NUMBER;
	EraseInit.NbSectors = 1U;
	EraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	HAL_FLASHEx_Erase(&EraseInit, &SectorError);
	BL_IWDG_Refresh();

	if(HAL_SUCCESSFUL_ERASE == SectorError)
	{
		ok = 1;
		for(i = 0; i < sizeof(FOTA_Meta_t); i++)
		{
			if(HAL_OK != HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, META_BASE_ADDRESS + i, ((const uint8_t *)&tmp)[i]))
			{
				ok = 0;
				break;
			}
		}
	}
	HAL_FLASH_Lock();
	return ok;
}

static uint8_t Bootloader_Slot_Range(uint8_t slot, uint32_t *base, uint32_t *end)
{
	if(slot == META_SLOT_A)
	{
		*base = SLOT_A_BASE_ADDRESS;
		*end  = SLOT_A_END_ADDRESS;
		return 1;
	}
	if(slot == META_SLOT_B)
	{
		*base = SLOT_B_BASE_ADDRESS;
		*end  = SLOT_B_END_ADDRESS;
		return 1;
	}
	return 0;
}

static uint8_t Bootloader_Is_App_Valid(uint32_t base, uint32_t end)
{
	uint32_t msp = *((volatile uint32_t *)base);
	uint32_t reset_handler = *((volatile uint32_t *)(base + 4U));

	return (msp >= SRAM1_BASE) && (msp <= STM32F4_SRAM1_END) &&
	       (reset_handler >= base) && (reset_handler < end);
}

/* Vector-table sanity is not enough: when the metadata records the image
 * length + CRC of the active slot, verify the WHOLE image at boot time so a
 * corrupted-but-vector-valid slot is treated as invalid (auto fallback /
 * rollback / erasable recovery). */
static uint8_t Bootloader_App_Trusted(uint8_t slot, uint32_t base, uint32_t end)
{
	FOTA_Meta_t meta;

	if(!Bootloader_Is_App_Valid(base, end))
	{
		return 0;
	}
	if(Meta_Read(&meta) && (meta.active_slot == slot) &&
	   (meta.image_len >= 8U) && (meta.image_len <= (end - base)))
	{
		return (Bootloader_Image_CRC_Verify(base, meta.image_len, meta.image_crc) == CRC_VERIFICATION_PASSED);
	}
	/* No recorded image info (fresh install / after rollback): trust the
	 * vector table only. */
	return 1;
}

static void Bootloader_Jump_To_App_At(uint32_t base)
{
	uint32_t MSP_Value = *((volatile uint32_t *)base);
	uint32_t MainAppAddr = *((volatile uint32_t *)(base + 4U));
	pMainApp ResetHandler_Address = (pMainApp)MainAppAddr;

	/* Start the independent watchdog: the app must refresh it in its main
	 * loop, otherwise a hung update rolls back automatically on the next
	 * (watchdog) reset. Once started it can only be stopped by POR. */
	BL_IWDG_Start();

	/* Disable all interrupts before switching context to the application */
	__disable_irq();

	/* Set Main Stack Pointer */
	__set_MSP(MSP_Value);

	/* DeInitialize the RCC clock configuration to the default reset state */
	HAL_RCC_DeInit();

	/* Jump to Application Reset Handler (the application re-configures VTOR itself) */
	ResetHandler_Address();
}

static uint8_t Bootloader_Resolve_Boot_Slot(void)
{
	FOTA_Meta_t meta;

	if(Meta_Read(&meta))
	{
		if(meta.state == META_STATE_TRIAL)
		{
			/* The last update was never confirmed by the app: roll back to
			 * the other slot and persist the decision. */
			meta.active_slot = (meta.active_slot == META_SLOT_A) ? META_SLOT_B : META_SLOT_A;
			meta.state = META_STATE_IDLE;
			meta.image_len = 0U;
			meta.image_crc = 0U;
			(void)Meta_Write(&meta);
		}
		return (meta.active_slot == META_SLOT_B) ? META_SLOT_B : META_SLOT_A;
	}
	/* Blank / corrupt metadata -> default to slot A */
	return META_SLOT_A;
}

void Bootloader_Jump_To_Active_App(void)
{
	uint8_t slot = Bootloader_Resolve_Boot_Slot();
	uint32_t base = 0;
	uint32_t end = 0;

	if(Bootloader_Slot_Range(slot, &base, &end) && Bootloader_App_Trusted(slot, base, end))
	{
		Bootloader_Jump_To_App_At(base);
		return;
	}

	/* Active slot invalid -> try the other slot and persist the switch */
	slot = (slot == META_SLOT_A) ? META_SLOT_B : META_SLOT_A;
	if(Bootloader_Slot_Range(slot, &base, &end) && Bootloader_App_Trusted(slot, base, end))
	{
		FOTA_Meta_t meta;
		if(Meta_Read(&meta))
		{
			meta.active_slot = slot;
			meta.state = META_STATE_IDLE;
			meta.image_len = 0U;
			meta.image_crc = 0U;
			(void)Meta_Write(&meta);
		}
		Bootloader_Jump_To_App_At(base);
	}
	/* No valid application anywhere: return, the caller waits for the HOST */
}

static uint8_t Bootloader_Image_CRC_Verify(uint32_t base_addr, uint32_t image_len, uint32_t expected)
{
	uint32_t calculated = Bootloader_CRC_Calculate((const uint8_t *)base_addr, image_len);
	return (calculated == expected) ? CRC_VERIFICATION_PASSED : CRC_VERIFICATION_FAILED;
}

static uint8_t Perform_Flash_Erase(uint8_t Sector_Numebr, uint8_t Number_Of_Sectors)
{
	uint8_t Sector_Validity_Status = INVALID_SECTOR_NUMBER;
	FLASH_EraseInitTypeDef pEraseInit;
	uint8_t Remaining_Sectors = 0;
	uint32_t SectorError = 0;

	if(Number_Of_Sectors > CBL_FLASH_MAX_SECTOR_NUMBER){
		/* Number Of sectors is out of range */
		Sector_Validity_Status = INVALID_SECTOR_NUMBER;
	}
	else{
		if((Sector_Numebr <= (CBL_FLASH_MAX_SECTOR_NUMBER - 1)) || (CBL_FLASH_MASS_ERASE == Sector_Numebr))
		{
			/* Check if user needs Mass erase */
			if(CBL_FLASH_MASS_ERASE == Sector_Numebr)
			{
				pEraseInit.TypeErase = FLASH_TYPEERASE_MASSERASE; /* Flash Mass erase activation */
			}
			else
			{
				/* User needs Sector erase */
				Remaining_Sectors = CBL_FLASH_MAX_SECTOR_NUMBER - Sector_Numebr;
				if(Number_Of_Sectors > Remaining_Sectors)
				{
					Number_Of_Sectors = Remaining_Sectors;
				}
				else { /* Nothing */ }

				pEraseInit.TypeErase = FLASH_TYPEERASE_SECTORS; /* Sectors erase only */
				pEraseInit.Sector = Sector_Numebr;        /* Initial FLASH sector to erase when Mass erase is disabled */
				pEraseInit.NbSectors = Number_Of_Sectors; /* Number of sectors to be erased. */
			}

			pEraseInit.Banks = FLASH_BANK_1; /* Bank 1  */
			pEraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* Device operating range: 2.7V to 3.6V */

			/* Unlock the FLASH control register access */
			HAL_FLASH_Unlock();
			if(CBL_FLASH_MASS_ERASE == Sector_Numebr)
			{
				/* Mass erase */
				HAL_FLASHEx_Erase(&pEraseInit, &SectorError);
				if(HAL_SUCCESSFUL_ERASE == SectorError){
					Sector_Validity_Status = SUCCESSFUL_ERASE;
				}
				else{
					Sector_Validity_Status = UNSUCCESSFUL_ERASE;
				}
				BL_IWDG_Refresh();
			}
			else
			{
				/* Sector erase one by one, refreshing the IWDG in between:
				 * the CPU stalls while the flash controller erases and the
				 * watchdog keeps counting on LSI. */
				Sector_Validity_Status = SUCCESSFUL_ERASE;
				for(uint8_t i = 0; i < Number_Of_Sectors; i++)
				{
					pEraseInit.Sector = Sector_Numebr + i;
					pEraseInit.NbSectors = 1U;
					HAL_FLASHEx_Erase(&pEraseInit, &SectorError);
					if(HAL_SUCCESSFUL_ERASE != SectorError)
					{
						Sector_Validity_Status = UNSUCCESSFUL_ERASE;
						break;
					}
					BL_IWDG_Refresh();
				}
			}
			/* Locks the FLASH control register access */
			HAL_FLASH_Lock();
		}
		else{
			Sector_Validity_Status = UNSUCCESSFUL_ERASE;
		}
	}
	return Sector_Validity_Status;
}

static void Bootloader_Erase_Flash(uint8_t *Host_Buffer)
{
	uint16_t Host_CMD_Packet_Len = 0;
    uint32_t Host_CRC32 = 0;
	uint8_t Erase_Status = INVALID_SECTOR_NUMBER;

	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32))
	{
		FOTA_Meta_t meta;
		uint8_t refuse = 0;
		uint32_t base = 0;
		uint32_t end = 0;

		/* Legacy command: erase slot A. Refuse only when slot A is the active
		 * slot AND holds a valid app (a corrupt active slot must stay
		 * recoverable). */
		if(Meta_Read(&meta) && (meta.active_slot == META_SLOT_A))
		{
			if(Bootloader_Slot_Range(META_SLOT_A, &base, &end) && Bootloader_App_Trusted(META_SLOT_A, base, end))
			{
				refuse = 1;
			}
		}
		if(refuse)
		{
			Erase_Status = INVALID_SECTOR_NUMBER;
		}
		else
		{
			Erase_Status = Perform_Flash_Erase(SLOT_A_SECTOR_START, SLOT_A_SECTOR_COUNT);
		}
		/* Report erase result */
		Bootloader_Send_Data_To_Host((uint8_t *)&Erase_Status, 1);
	}
	else{
		/* Send Not acknowledge to the HOST */
		Bootloader_Send_NACK();
	}
}

static void Bootloader_Get_Slot_Status(uint8_t *Host_Buffer)
{
	/* Packet: [5][0x15][CRC32]  ->  Reply: [active_slot][state][image_len LE32] */
	uint16_t Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	uint32_t Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	uint8_t Reply[6] = {META_SLOT_A, META_STATE_EMPTY, 0, 0, 0, 0};

	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0], Host_CMD_Packet_Len - 4, Host_CRC32))
	{
		FOTA_Meta_t meta;
		if(Meta_Read(&meta))
		{
			Reply[0] = meta.active_slot;
			Reply[1] = meta.state;
			*((uint32_t *)(&Reply[2])) = meta.image_len;
		}
		Bootloader_Send_Data_To_Host(Reply, sizeof(Reply));
	}
	else
	{
		Bootloader_Send_NACK();
	}
}

static void Bootloader_Erase_Slot(uint8_t *Host_Buffer)
{
	/* Packet: [6][0x16][slot][CRC32]  ->  Reply: erase status (1 byte) */
	uint16_t Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	uint32_t Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	uint8_t Erase_Status = INVALID_SECTOR_NUMBER;
	uint8_t slot = Host_Buffer[2];

	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0], Host_CMD_Packet_Len - 4, Host_CRC32))
	{
		FOTA_Meta_t meta;
		uint8_t refuse = 0;
		uint32_t base = 0;
		uint32_t end = 0;

		/* Refuse only when the target is the active slot AND holds a valid
		 * app; a corrupt active slot must stay erasable for recovery. */
		if(Meta_Read(&meta) && (slot == meta.active_slot))
		{
			if(Bootloader_Slot_Range(slot, &base, &end) && Bootloader_App_Trusted(slot, base, end))
			{
				refuse = 1;
			}
		}
		if(refuse)
		{
			Erase_Status = INVALID_SECTOR_NUMBER;
		}
		else if(slot == META_SLOT_A)
		{
			Erase_Status = Perform_Flash_Erase(SLOT_A_SECTOR_START, SLOT_A_SECTOR_COUNT);
		}
		else if(slot == META_SLOT_B)
		{
			Erase_Status = Perform_Flash_Erase(SLOT_B_SECTOR_START, SLOT_B_SECTOR_COUNT);
		}
		Bootloader_Send_Data_To_Host(&Erase_Status, 1);
	}
	else
	{
		Bootloader_Send_NACK();
	}
}

#if BL_REQUIRE_SIGNED_IMAGES
/* Verify the signed-package trailer stored right after the image:
 * TLV parse -> SHA-256(image || security_version_LE32) -> compare ->
 * ECDSA-P256 verify against the compiled-in public key.
 * Returns BL_ACT_STATUS_SUCCESS or the specific BL_ACT_FAIL_* code. */
static uint8_t Bootloader_Verify_Signed_Image(uint32_t base, uint32_t image_len, uint32_t slot_end,
                                               uint32_t *sv_out, uint32_t *verify_ms_out)
{
	Image_TLV_Info_t tlv;
	SHA256_CTX sha;
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint8_t sv_le[IMAGE_TLV_SV_LEN];
	uint32_t cycles_start;

	/* DWT cycle counter: measure the whole chain for the bring-up log. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	DWT->CYCCNT = 0U;
	cycles_start = DWT->CYCCNT;

	BL_IWDG_Refresh();
	if(!Image_TLV_Parse(base + image_len, slot_end - (base + image_len), &tlv))
	{
		return BL_ACT_FAIL_NO_TLV;
	}

	/* digest = SHA-256(image || security_version_LE32), matching tools/sign.py.
	 * The security version is covered by the digest AND the signature, so a
	 * stripped or forged rollback counter cannot pass activation. */
	BL_IWDG_Refresh();
	sv_le[0] = (uint8_t)(tlv.security_version >> 0);
	sv_le[1] = (uint8_t)(tlv.security_version >> 8);
	sv_le[2] = (uint8_t)(tlv.security_version >> 16);
	sv_le[3] = (uint8_t)(tlv.security_version >> 24);
	sha256_init(&sha);
	sha256_update(&sha, (const uint8_t *)base, image_len);
	sha256_update(&sha, sv_le, sizeof(sv_le));
	sha256_final(&sha, digest);

	if(memcmp(digest, tlv.digest, SHA256_DIGEST_SIZE) != 0)
	{
		return BL_ACT_FAIL_DIGEST;
	}

	BL_IWDG_Refresh();
	if(uECC_verify(BL_PUBLIC_KEY_XY, digest, SHA256_DIGEST_SIZE, tlv.signature, uECC_secp256r1()) != 1)
	{
		return BL_ACT_FAIL_SIGNATURE;
	}

	*sv_out = tlv.security_version;
	*verify_ms_out = (DWT->CYCCNT - cycles_start) / (SystemCoreClock / 1000U);
	return BL_ACT_STATUS_SUCCESS;
}
#endif /* BL_REQUIRE_SIGNED_IMAGES */

/* Human-readable activate failure reason for the log line. */
static const char *BL_Act_Reason(uint8_t status)
{
	switch(status)
	{
		case BL_ACT_FAIL_VALIDATION: return " (slot/image bounds/vector/CRC checks)";
		case BL_ACT_FAIL_NO_TLV:     return " (no or malformed TLV signature trailer)";
		case BL_ACT_FAIL_DIGEST:     return " (digest mismatch: image or sv altered)";
		case BL_ACT_FAIL_SIGNATURE:  return " (ECDSA-P256 signature invalid)";
		case BL_ACT_FAIL_SEC_VERSION: return " (security version below anti-rollback floor)";
		default:                     return "";
	}
}

/* Printf-style log on the host UART. Only called AFTER the protocol reply of
 * the current command has been sent, so the host's exact-length reads never
 * see log bytes as stale reply data (the host drains them as text). */
static void BL_Log(const char *fmt, ...)
{
	static char log_buf[112];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
	va_end(args);

	if(n > 0)
	{
		if(n > (int)(sizeof(log_buf) - 1U))
		{
			n = (int)(sizeof(log_buf) - 1U);
		}
		BL_Send_Raw((const uint8_t *)log_buf, (uint32_t)n);
	}
}

static void Bootloader_Activate(uint8_t *Host_Buffer)
{
	/* Packet: [14][0x17][slot][image_len LE32][image_crc LE32][CRC32]
	 * Validation chain: slot range -> image bounds -> vector sanity ->
	 * whole-image CRC -> signature trailer (BL_REQUIRE_SIGNED_IMAGES).
	 * The 1-byte status reply is sent BEFORE any log line. */
	uint16_t Host_CMD_Packet_Len = 0;
	uint32_t Host_CRC32 = 0;
	uint8_t Status = BL_ACT_FAIL_VALIDATION;

	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32))
	{
		BL_IWDG_Refresh();
		uint8_t slot = Host_Buffer[2];
		uint32_t image_len = *((uint32_t *)(&Host_Buffer[3]));
		uint32_t image_crc = *((uint32_t *)(&Host_Buffer[7]));
		uint32_t base = 0;
		uint32_t end = 0;
#if BL_REQUIRE_SIGNED_IMAGES
		uint32_t sv = 0U;
		uint32_t verify_ms = 0U;
#endif

		if(Bootloader_Slot_Range(slot, &base, &end) &&
		   (image_len >= 8U) && (image_len <= (end - base)) &&
		   Bootloader_Is_App_Valid(base, end) &&
		   (CRC_VERIFICATION_PASSED == Bootloader_Image_CRC_Verify(base, image_len, image_crc)))
		{
#if BL_REQUIRE_SIGNED_IMAGES
			Status = Bootloader_Verify_Signed_Image(base, image_len, end, &sv, &verify_ms);
			if(BL_ACT_STATUS_SUCCESS == Status)
			{
				/* Anti-rollback floor (feature B): min_sv only ever rises. A
				 * validly-signed but OLDER image (sv below the floor) is a
				 * downgrade attack, rejected here. Rolling back an
				 * unconfirmed TRIAL slot never reaches this check. */
				FOTA_Meta_t cur_meta;

				if(Meta_Read(&cur_meta) && (sv < cur_meta.min_sv))
				{
					Status = BL_ACT_FAIL_SEC_VERSION;
				}
			}
#else
			/* Debug build: unsigned packages activate. Never ship this. */
			Status = BL_ACT_STATUS_SUCCESS;
#endif
		}
		else
		{
			Status = BL_ACT_FAIL_VALIDATION;
		}

		if(BL_ACT_STATUS_SUCCESS == Status)
		{
			FOTA_Meta_t meta;
			memset(&meta, 0, sizeof(meta));
			meta.magic = META_MAGIC;
			meta.version = META_VERSION;
			meta.active_slot = slot;
			meta.state = META_STATE_TRIAL;
			meta.image_len = image_len;
			meta.image_crc = image_crc;
#if BL_REQUIRE_SIGNED_IMAGES
			meta.min_sv = sv;   /* raise the floor; never lowered elsewhere */
#else
			meta.min_sv = 0U;   /* debug build: no anti-rollback guarantee */
#endif

			if(Meta_Write(&meta))
			{
				Bootloader_Send_Data_To_Host(&Status, 1);
#if BL_REQUIRE_SIGNED_IMAGES
				BL_Log("[BL] activate slot=%c: signature OK (sv=%lu, verify %lu ms), jumping\r\n",
				       (slot == META_SLOT_B) ? 'B' : 'A', (unsigned long)sv, (unsigned long)verify_ms);
#endif
				Bootloader_Jump_To_App_At(base);
				return;
			}
			Status = BL_ACT_FAIL_VALIDATION;
		}

		Bootloader_Send_Data_To_Host(&Status, 1);
		BL_Log("[BL] activate rejected: 0x%02X%s\r\n", Status, BL_Act_Reason(Status));
	}
	else
	{
		Bootloader_Send_NACK();
	}
}

static uint8_t Flash_Memory_Write_Payload(uint8_t *Host_Payload, uint32_t Payload_Start_Address, uint16_t Payload_Len)
{
	HAL_StatusTypeDef HAL_Status = HAL_ERROR;
	uint8_t Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;
	uint16_t Payload_Counter = 0;

	/* Unlock the FLASH control register access */
	HAL_Status = HAL_FLASH_Unlock();

	if(HAL_Status != HAL_OK){
		Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;
	}
	else{
		for(Payload_Counter = 0; Payload_Counter < Payload_Len; Payload_Counter++){
			/* Program a byte at a specified address */
			HAL_Status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, Payload_Start_Address + Payload_Counter, Host_Payload[Payload_Counter]);
			if(HAL_Status != HAL_OK){
				Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;
				break;
			}
			else{
				Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_PASSED;
			}
		}
	}

	if((FLASH_PAYLOAD_WRITE_PASSED == Flash_Payload_Write_Status) && (HAL_OK == HAL_Status)){
		/* Locks the FLASH control register access */
		HAL_Status = HAL_FLASH_Lock();
		if(HAL_Status != HAL_OK){
			Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;
		}
		else{
			Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_PASSED;
		}
	}
	else{
		Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;
	}

	return Flash_Payload_Write_Status;
}

static void Bootloader_Memory_Write(uint8_t *Host_Buffer)
{
	uint16_t Host_CMD_Packet_Len = 0;
    uint32_t Host_CRC32 = 0;
	uint32_t HOST_Address = 0;
	uint8_t Payload_Len = 0;
	uint8_t Address_Verification = ADDRESS_IS_INVALID;
	uint8_t Flash_Payload_Write_Status = FLASH_PAYLOAD_WRITE_FAILED;

	/* Extract the CRC32 and packet length sent by the HOST */
	Host_CMD_Packet_Len = Host_Buffer[0] + 1;
	Host_CRC32 = *((uint32_t *)((Host_Buffer + Host_CMD_Packet_Len) - CRC_TYPE_SIZE_BYTE));
	/* CRC Verification */
	if(CRC_VERIFICATION_PASSED == Bootloader_CRC_Verify((uint8_t *)&Host_Buffer[0] , Host_CMD_Packet_Len - 4, Host_CRC32)){
		/* Extract the start address from the Host packet */
		HOST_Address = *((uint32_t *)(&Host_Buffer[2]));
		/* Extract the payload length from the Host packet */
		Payload_Len = Host_Buffer[6];
		/* Verify the Extracted address and payload range */
		Address_Verification = Host_Address_Verification(HOST_Address, Payload_Len);
		if(ADDRESS_IS_VALID == Address_Verification){
			/* Write the payload to the Flash memory */
			Flash_Payload_Write_Status = Flash_Memory_Write_Payload((uint8_t *)&Host_Buffer[7], HOST_Address, Payload_Len);
			/* Report payload write result */
			Bootloader_Send_Data_To_Host((uint8_t *)&Flash_Payload_Write_Status, 1);
		}
		else{
			/* Report address verification failed */
			Bootloader_Send_Data_To_Host((uint8_t *)&Address_Verification, 1);
		}
	}
	else{
		/* Send Not acknowledge to the HOST */
		Bootloader_Send_NACK();
	}
}

static uint8_t Host_Address_Verification(uint32_t Jump_Address, uint16_t Payload_Len)
{
	uint8_t Address_Verification = ADDRESS_IS_INVALID;

	if((Jump_Address >= SRAM1_BASE) && (Jump_Address <= STM32F4_SRAM1_END))
	{
		Address_Verification = ADDRESS_IS_VALID;
	}
	else if((Jump_Address >= SLOT_A_BASE_ADDRESS) && ((Jump_Address + Payload_Len) <= SLOT_A_END_ADDRESS))
	{
		/* FOTA writes are restricted to slot A / slot B, so the bootloader
		 * (sectors 0-1) and the metadata sector (2) can never be overwritten. */
		Address_Verification = ADDRESS_IS_VALID;
	}
	else if((Jump_Address >= SLOT_B_BASE_ADDRESS) && ((Jump_Address + Payload_Len) <= SLOT_B_END_ADDRESS))
	{
		Address_Verification = ADDRESS_IS_VALID;
	}
	else
	{
		Address_Verification = ADDRESS_IS_INVALID;
	}
	return Address_Verification;
}

uint8_t Bootloader_CRC_Verify(uint8_t *pData, uint32_t Data_Len, uint32_t Host_CRC)
{
	return (Bootloader_CRC_Calculate(pData, Data_Len) == Host_CRC) ? CRC_VERIFICATION_PASSED : CRC_VERIFICATION_FAILED;
}

uint32_t Bootloader_CRC_Calculate(const uint8_t *pData, uint32_t Data_Len)
{
	uint32_t MCU_CRC_Calculated = 0;
	uint32_t Data_Buffer = 0;
	uint32_t Data_Counter = 0;

	/* Calculate CRC32 (byte fed as a 32-bit word, matches the HOST software
	 * implementation and the original ESP32 sketch) */
	__HAL_CRC_DR_RESET(CRC_ENGINE_OBJ);
	for(Data_Counter = 0; Data_Counter < Data_Len; Data_Counter++){
		Data_Buffer = (uint32_t)pData[Data_Counter];
		MCU_CRC_Calculated = HAL_CRC_Accumulate(CRC_ENGINE_OBJ, &Data_Buffer, 1);
	}
	/* Reset the CRC Calculation Unit */
	__HAL_CRC_DR_RESET(CRC_ENGINE_OBJ);
	return MCU_CRC_Calculated;
}

static void Bootloader_Send_NACK(void)
{
	uint8_t Ack_Value = CBL_SEND_NACK;
	BL_Send_Raw(&Ack_Value, 1);
}

static void Bootloader_Send_Data_To_Host(uint8_t *Host_Buffer, uint32_t Data_Len)
{
	BL_Send_Raw(Host_Buffer, Data_Len);
}

/************************************    IWDG (Independent Watchdog)    ************************************/
static void BL_IWDG_Start(void)
{
	/* Sequence per RM0090: unlock PR/RLR, set prescaler + reload, reload,
	 * then start. Writing the start key on an already-running IWDG is
	 * harmless (it only starts it the first time). */
	IWDG->KR = 0x5555U;                         /* enable register access */
	IWDG->PR = BL_IWDG_PRESCALER_VALUE;         /* /256 -> ~10 s with RLR below */
	IWDG->RLR = BL_IWDG_RELOAD_VALUE;
	IWDG->KR = BL_IWDG_REFRESH_KEY;             /* reload the counter */
	IWDG->KR = 0xCCCCU;                         /* start (only has effect once) */
}

void BL_IWDG_Refresh(void)
{
	/* Reload the counter; harmless when the watchdog is not running. */
	IWDG->KR = BL_IWDG_REFRESH_KEY;
}
