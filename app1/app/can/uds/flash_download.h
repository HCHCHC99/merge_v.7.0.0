#ifndef FLASH_DOWNLOAD_H
#define FLASH_DOWNLOAD_H
#include "memory_map.h"

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_PREPARING,
    FW_UPDATE_READY,
    FW_UPDATE_TRANSFERRING,
    FW_UPDATE_VERIFYING,
    FW_UPDATE_COMPLETE,
    FW_UPDATE_ERROR
} FlashDownloadState_t;

typedef enum
{
    FW_RESULT_OK = 0,
    FW_RESULT_ADDR_INVALID,
    FW_RESULT_SIZE_TOO_LARGE,
    FW_RESULT_ERASE_FAILED,
    FW_RESULT_WRITE_FAILED,
    FW_RESULT_VERIFY_FAILED,
    FW_RESULT_SEQUENCE_ERROR,
    FW_RESULT_BUSY,             // æ
    FW_RESULT_NOT_READY
} FlashDownloadResult_t;

typedef struct
{
    uint32_t total_size;
    uint32_t received_size;
    uint32_t target_address;
    uint8_t progress_percent;
} FlashDownloadProgress_t;

typedef struct
{
    uint32_t max_firmware_size;
    uint32_t flash_sector_size;
    uint32_t user_start_addr;
    uint32_t user_end_addr;
    uint8_t verify_enabled;
    uint8_t auto_reset_on_complete;
} FlashDownloadConfig_t;



/**/

/**/


/**/
#define FW_FLASH_WRITE_ENABLED      1


/**
 */
FlashDownloadResult_t FlashDownload_OnRequestDownload(uint32_t address, uint32_t size);

/**
 */
FlashDownloadResult_t FlashDownload_OnTransferData(uint8_t block_sequence_number,
                                                    uint8_t* data,
                                                    uint16_t len);

/**
 */
FlashDownloadResult_t FlashDownload_OnTransferExit(void);

/**
 */
FlashDownloadResult_t FlashDownload_Erase(uint32_t address, uint32_t size);

/**
 */
FlashDownloadResult_t FlashDownload_CalculateCRC(uint32_t address, uint32_t size, uint32_t* crc_result);


/**
 */
FlashDownloadState_t FlashDownload_GetState(void);

/**
 */
void FlashDownload_GetProgress(FlashDownloadProgress_t* progress);

/**
 */
FlashDownloadResult_t FlashDownload_GetLastError(void);

/**
 */
uint16_t FlashDownload_GetFirmwareVersion(void);

/**
 */
uint16_t FlashDownload_GetBootloaderVersion(void);

/**
 */
uint32_t FlashDownload_GetFirmwareCRC(void);


/**
 */
void FlashDownload_Init(const FlashDownloadConfig_t* config);

/**
 */
void FlashDownload_Cancel(void);

/**
 */
void FlashDownload_Reset(void);


/**
 */
void FlashDownload_Task(void);

/**
 */
bool FlashDownload_IsPending(void);

#endif /* FLASH_DOWNLOAD_H */
