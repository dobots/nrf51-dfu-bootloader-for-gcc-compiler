/* Copyright (c) 2013 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include <stddef.h>
#include "dfu.h"
#include <dfu_types.h>
#include "dfu_bank_internal.h"
#include "nrf.h"
#include "nrf_sdm.h"
#include "app_error.h"
#include "app_timer.h"
#include "bootloader.h"
#include "bootloader_types.h"
#include "pstorage.h"
#include "nrf_mbr.h"
#include "dfu_init.h"
#include "sdk_common.h"

#include "serial.h"

static dfu_state_t                  m_dfu_state;                /**< Current DFU state. */
static uint32_t                     m_image_size;               /**< Size of the image that will be transmitted. */

static dfu_start_packet_t           m_start_packet;             /**< Start packet received for this update procedure. Contains update mode and image sizes information to be used for image transfer. */
static uint8_t                      m_init_packet[128];         /**< Init packet, can hold CRC, Hash, Signed Hash and similar, for image validation, integrety check and authorization checking. */
static uint8_t                      m_init_packet_length;       /**< Length of init packet received. */
static uint16_t                     m_image_crc;                /**< Calculated CRC of the image received. */

//static app_timer_id_t               m_dfu_timer_id;             /**< Application timer id. */
APP_TIMER_DEF(m_dfu_timer_id);                                  /**< Application timer id. */
static bool                         m_dfu_timed_out = false;    /**< Boolean flag value for tracking DFU timer timeout state. */

static pstorage_handle_t            m_storage_handle_swap;      /**< Pstorage handle for the swap area (bank 1). Bank used when updating an application or bootloader without SoftDevice. */
static pstorage_handle_t            m_storage_handle_app;       /**< Pstorage handle for the application area (bank 0). Bank used when updating a SoftDevice w/wo bootloader. Handle also used when swapping received application from bank 1 to bank 0. */
static pstorage_handle_t          * mp_storage_handle_active;   /**< Pointer to the pstorage handle for the active bank for receiving of data packets. */

static dfu_callback_t               m_data_pkt_cb;              /**< Callback from DFU Bank module for notification of asynchronous operation such as flash prepare. */
static dfu_bank_func_t              m_functions;                /**< Structure holding operations for the selected update process. */


/**@brief Function for handling callbacks from pstorage module.
 *
 * @details Handles pstorage results for clear and storage operation. For detailed description of
 *          the parameters provided with the callback, please refer to \ref pstorage_ntf_cb_t.
 */
static void pstorage_callback_handler(pstorage_handle_t * p_handle,
                                      uint8_t             op_code,
                                      uint32_t            result,
                                      uint8_t           * p_data,
                                      uint32_t            data_len)
{

#ifdef VERBOSE
    /*
    WRITE_VERBOSE("pstore cb hand\r\n", 17);
    char decText[8] = {0};
    get_dec_str(decText, 7, op_code);
    WRITE_VERBOSE("opcode = ", 10);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, result);
    WRITE_VERBOSE("result = ", 10);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
    */
#endif
    switch (op_code)
    {
        case PSTORAGE_STORE_OP_CODE:
            if ((m_dfu_state == DFU_STATE_RX_DATA_PKT) && (m_data_pkt_cb != NULL))
            {
                m_data_pkt_cb(DATA_PACKET, result, p_data);
            }
            break;

        case PSTORAGE_CLEAR_OP_CODE:
            WRITE_VERBOSE("pstorage cb handle clear\r\n", 27);
            if (m_dfu_state == DFU_STATE_PREPARING)
            {
                m_functions.cleared();
                m_dfu_state = DFU_STATE_RDY;
                if (m_data_pkt_cb != NULL)
                {
                    m_data_pkt_cb(START_PACKET, result, p_data);
                }
            }
            break;

        default:
            break;
    }
    APP_ERROR_CHECK(result);
}


/**@brief Function for handling the DFU timeout.
 *
 * @param[in] p_context The timeout context.
 */
static void dfu_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    dfu_update_status_t update_status;

    m_dfu_timed_out           = true;
    update_status.status_code = DFU_TIMEOUT;

    bootloader_dfu_update_process(update_status);
}


/**@brief   Function for restarting the DFU Timer.
 *
 * @details This function will stop and restart the DFU timer. This function will be called by the
 *          functions handling any DFU packet received from the peer that is transferring a firmware
 *          image.
 */
static uint32_t dfu_timer_restart(void)
{
    if (m_dfu_timed_out)
    {
        // The DFU timer had already timed out.
        return NRF_ERROR_INVALID_STATE;
    }

    uint32_t err_code = app_timer_stop(m_dfu_timer_id);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_start(m_dfu_timer_id, DFU_TIMEOUT_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);

    return err_code;
}


/**@brief   Function for preparing of flash before receiving SoftDevice image.
 *
 * @details This function will erase current application area to ensure sufficient amount of
 *          storage for the SoftDevice image. Upon erase complete a callback will be done.
 *          See \ref dfu_bank_prepare_t for further details.
 */
static void dfu_prepare_func_app_erase(uint32_t image_size)
{
    uint32_t err_code;

    mp_storage_handle_active = &m_storage_handle_app;

    // Doing a SoftDevice update thus current application must be cleared to ensure enough space
    // for new SoftDevice.
    m_dfu_state = DFU_STATE_PREPARING;
    err_code    = pstorage_clear(&m_storage_handle_app, m_image_size);
    APP_ERROR_CHECK(err_code);
}


/**@brief   Function for preparing swap before receiving application or bootloader image.
 *
 * @details This function will erase current swap area to ensure flash is ready for storage of the
 *          Application or Bootloader image. Upon erase complete a callback will be done.
 *          See \ref dfu_bank_prepare_t for further details.
 */
static void dfu_prepare_func_swap_erase(uint32_t image_size)
{
    uint32_t err_code;
    WRITE_VERBOSE("swap erase\r\n", 13);
    mp_storage_handle_active = &m_storage_handle_swap;

    m_dfu_state = DFU_STATE_PREPARING;
    err_code    = pstorage_clear(&m_storage_handle_swap, DFU_IMAGE_MAX_SIZE_BANKED);
    APP_ERROR_CHECK(err_code);
}


/**@brief   Function for handling behaviour when clear operation has completed.
 */
static void dfu_cleared_func_swap(void)
{
    WRITE_VERBOSE("swap erase done\r\n", 18);
    // Do nothing.
}


/**@brief   Function for handling behaviour when clear operation has completed.
 */
static void dfu_cleared_func_app(void)
{
    dfu_update_status_t update_status = {DFU_BANK_0_ERASED, };
    bootloader_dfu_update_process(update_status);
}


/**@brief   Function for calculating storage offset for receiving SoftDevice image.
 *
 * @details When a new SoftDevice is received it will be temporary stored in flash before moved to
 *          address 0x0. In order to succesfully validate transfer and relocation it is important
 *          that temporary image and final installed image does not ovwerlap hence an offset must
 *          be calculated in case new image is larger than currently installed SoftDevice.
 */
uint32_t offset_calculate(uint32_t sd_image_size)
{
    uint32_t offset = 0;

    if (m_start_packet.sd_image_size > DFU_BANK_0_REGION_START)
    {
        uint32_t page_mask = (CODE_PAGE_SIZE - 1);
        uint32_t diff = m_start_packet.sd_image_size - DFU_BANK_0_REGION_START;

        offset = diff & ~page_mask;

        // Align offset to next page if image size is not page sized.
        if ((diff & page_mask) > 0)
        {
            offset += CODE_PAGE_SIZE;
        }
    }

    return offset;
}


/**@brief Function for activating received SoftDevice image.
 *
 *  @note This function will not move the SoftDevice image.
 *        The bootloader settings will be marked as SoftDevice update complete and the swapping of
 *        current SoftDevice will occur after system reset.
 *
 * @return NRF_SUCCESS on success.
 */
static uint32_t dfu_activate_sd(void)
{
    dfu_update_status_t update_status;

    update_status.status_code    = DFU_UPDATE_SD_COMPLETE;
    update_status.app_crc        = m_image_crc;
    update_status.sd_image_start = DFU_BANK_0_REGION_START;
    update_status.sd_size        = m_start_packet.sd_image_size;
    update_status.bl_size        = m_start_packet.bl_image_size;
    update_status.app_size       = m_start_packet.app_image_size;

    bootloader_dfu_update_process(update_status);

    return NRF_SUCCESS;
}


/**@brief Function for activating received Application image.
 *
 *  @details This function will move the received application image fram swap (bank 1) to
 *           application area (bank 0).
 *
 * @return NRF_SUCCESS on success. Error code otherwise.
 */
static uint32_t dfu_activate_app(void)
{
    WRITE_VERBOSE("act app\r\n", 10);
    uint32_t err_code;

    // Erase BANK 0.
    err_code = pstorage_clear(&m_storage_handle_app, m_start_packet.app_image_size);
    APP_ERROR_CHECK(err_code);

#ifdef VERBOSE
    char decText[8] = {0};
    get_dec_str(decText, 7, err_code);
    WRITE_VERBOSE("pstorage_clear = ", 17);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, m_storage_handle_app.block_id);
    WRITE_VERBOSE("dest = ", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, m_storage_handle_swap.block_id);
    WRITE_VERBOSE("src = ", 7);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, m_start_packet.app_image_size);
    WRITE_VERBOSE("size = ", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
#endif

    err_code = pstorage_store(&m_storage_handle_app,
                                  (uint8_t *)m_storage_handle_swap.block_id,
                                  m_start_packet.app_image_size,
                                  0);

#ifdef VERBOSE
    //char decText[8] = {0};
    get_dec_str(decText, 7, err_code);
    WRITE_VERBOSE("pstorage_store = ", 17);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
#endif

    if (err_code == NRF_SUCCESS)
    {
        dfu_update_status_t update_status;

        memset(&update_status, 0, sizeof(dfu_update_status_t ));
        update_status.status_code = DFU_UPDATE_APP_COMPLETE;
        update_status.app_crc     = m_image_crc;
        update_status.app_size    = m_start_packet.app_image_size;

        bootloader_dfu_update_process(update_status);
    }

    return err_code;
}


/**@brief Function for activating received Bootloader image.
 *
 *  @note This function will not move the bootloader image.
 *        The bootloader settings will be marked as Bootloader update complete and the swapping of
 *        current bootloader will occur after system reset.
 *
 * @return NRF_SUCCESS on success.
 */
static uint32_t dfu_activate_bl(void)
{
    dfu_update_status_t update_status;

    update_status.status_code = DFU_UPDATE_BOOT_COMPLETE;
    update_status.app_crc     = m_image_crc;
    update_status.sd_size     = m_start_packet.sd_image_size;
    update_status.bl_size     = m_start_packet.bl_image_size;
    update_status.app_size    = m_start_packet.app_image_size;

    bootloader_dfu_update_process(update_status);

    return NRF_SUCCESS;
}


uint32_t dfu_init(void)
{
    uint32_t                err_code;
    pstorage_module_param_t storage_module_param = {.cb = pstorage_callback_handler};

    m_init_packet_length = 0;
    m_image_crc          = 0;

WRITE_VERBOSE("pstorage_re\r\n", 14);
    err_code = pstorage_register(&storage_module_param, &m_storage_handle_app); // returns 4


    if (err_code != NRF_SUCCESS)
    {
        m_dfu_state = DFU_STATE_INIT_ERROR;
        return err_code;
    }

    m_storage_handle_app.block_id  = DFU_BANK_0_REGION_START;
    m_storage_handle_swap          = m_storage_handle_app;
    m_storage_handle_swap.block_id = DFU_BANK_1_REGION_START;

    // Create the timer to monitor the activity by the peer doing the firmware update.
WRITE_VERBOSE("app_timer_c\r\n", 14);
    err_code = app_timer_create(&m_dfu_timer_id,
                                APP_TIMER_MODE_SINGLE_SHOT,
                                dfu_timeout_handler);
    APP_ERROR_CHECK(err_code);

    // Start the DFU timer.
WRITE_VERBOSE("app_timer_s\r\n", 14);
    err_code = app_timer_start(m_dfu_timer_id, DFU_TIMEOUT_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);

    m_data_received = 0;
    m_dfu_state     = DFU_STATE_IDLE;

    return NRF_SUCCESS;
}


void dfu_register_callback(dfu_callback_t callback_handler)
{
    m_data_pkt_cb = callback_handler;
}


uint32_t dfu_start_pkt_handle(dfu_update_packet_t * p_packet)
{
WRITE_VERBOSE("start_pkt_handle\r\n", 19);
    uint32_t err_code;

    m_start_packet = *(p_packet->params.start_packet);

    // Check that the requested update procedure is supported.
    // Currently the following combinations are allowed:
    // - Application
    // - SoftDevice
    // - Bootloader
    // - SoftDevice with Bootloader
    if (IS_UPDATING_APP(m_start_packet) &&
        (IS_UPDATING_SD(m_start_packet) || IS_UPDATING_BL(m_start_packet)))
    {
        // App update is only supported independently.
        return NRF_ERROR_NOT_SUPPORTED;
    }

    if (!(IS_WORD_SIZED(m_start_packet.sd_image_size) &&
          IS_WORD_SIZED(m_start_packet.bl_image_size) &&
          IS_WORD_SIZED(m_start_packet.app_image_size)))
    {
        // Image_sizes are not a multiple of 4 (word size).
        return NRF_ERROR_NOT_SUPPORTED;
    }

    m_image_size = m_start_packet.sd_image_size + m_start_packet.bl_image_size +
                   m_start_packet.app_image_size;

    if (m_start_packet.bl_image_size > DFU_BL_IMAGE_MAX_SIZE)
    {
        return NRF_ERROR_DATA_SIZE;
    }

    if (IS_UPDATING_SD(m_start_packet))
    {
        if (m_image_size > (DFU_IMAGE_MAX_SIZE_FULL))
        {
            return NRF_ERROR_DATA_SIZE;
        }
        m_functions.prepare  = dfu_prepare_func_app_erase;
        m_functions.cleared  = dfu_cleared_func_app;
        m_functions.activate = dfu_activate_sd;
    }
    else
    {
        if (m_image_size > DFU_IMAGE_MAX_SIZE_BANKED)
        {
            return NRF_ERROR_DATA_SIZE;
        }

        m_functions.prepare = dfu_prepare_func_swap_erase;
        m_functions.cleared = dfu_cleared_func_swap;
        if (IS_UPDATING_BL(m_start_packet))
        {
            m_functions.activate = dfu_activate_bl;
        }
        else
        {
            m_functions.activate = dfu_activate_app;
        }
    }

    switch (m_dfu_state)
    {
        case DFU_STATE_IDLE:
            // Valid peer activity detected. Hence restart the DFU timer.
            err_code = dfu_timer_restart();
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }
            m_functions.prepare(m_image_size);

            break;

        default:
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }

    return err_code;
}


uint32_t dfu_data_pkt_handle(dfu_update_packet_t * p_packet)
{
    uint32_t   data_length;
    uint32_t   err_code;
    uint32_t * p_data;

    if (p_packet == NULL)
    {
        return NRF_ERROR_NULL;
    }

    // Check pointer alignment.
    if (!is_word_aligned(p_packet->params.data_packet.p_data_packet))
    {
        // The p_data_packet is not word aligned address.
        return NRF_ERROR_INVALID_ADDR;
    }

    switch (m_dfu_state)
    {
        case DFU_STATE_RDY:
        case DFU_STATE_RX_INIT_PKT:
            return NRF_ERROR_INVALID_STATE;

        case DFU_STATE_RX_DATA_PKT:
            data_length = p_packet->params.data_packet.packet_length * sizeof(uint32_t);

            if ((m_data_received + data_length) > m_image_size)
            {
                // The caller is trying to write more bytes into the flash than the size provided to
                // the dfu_image_size_set function. This is treated as a serious error condition and
                // an unrecoverable one. Hence point the variable mp_app_write_address to the top of
                // the flash area. This will ensure that all future application data packet writes
                // will be blocked because of the above check.
                m_data_received = 0xFFFFFFFF;

                return NRF_ERROR_DATA_SIZE;
            }

            // Valid peer activity detected. Hence restart the DFU timer.
            err_code = dfu_timer_restart();
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }

            p_data = (uint32_t *)p_packet->params.data_packet.p_data_packet;

            err_code = pstorage_store(mp_storage_handle_active,
                                          (uint8_t *)p_data,
                                          data_length,
                                          m_data_received);

//char decText[8] = {0};

//get_dec_str(decText, 7, mp_storage_handle_active->block_id);
//WRITE_VERBOSE(decText, 7);
//WRITE_VERBOSE("\r\n", 3);

//for (int i=0; i<data_length; i++) {
//get_dec_str(decText, 3, ((uint8_t*)p_data)[i]);
//WRITE_VERBOSE(decText, 4);
//}
//WRITE_VERBOSE("\r\n", 3);
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }

            m_data_received += data_length;

            if (m_data_received != m_image_size)
            {
                // The entire image is not received yet. More data is expected.
                err_code = NRF_ERROR_INVALID_LENGTH;
            }
            else
            {
                // The entire image has been received. Return NRF_SUCCESS.
                err_code = NRF_SUCCESS;
            }
            break;

        default:
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }

    return err_code;
}


uint32_t dfu_init_pkt_complete(void)
{
    uint32_t err_code = NRF_ERROR_INVALID_STATE;

    // DFU initialization has been done and a start packet has been received.
    if (IMAGE_WRITE_IN_PROGRESS())
    {
        // Image write is already in progress. Cannot handle an init packet now.
        return NRF_ERROR_INVALID_STATE;
    }

WRITE_VERBOSE("init_pkt_complete\r\n", 19);
#ifdef VERBOSE
    char decText[8] = {0};
    get_dec_str(decText, 7, m_dfu_state);
    WRITE_VERBOSE("state = ", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
#endif
    if (m_dfu_state == DFU_STATE_RX_INIT_PKT)
    {
        err_code = dfu_init_prevalidate(m_init_packet, m_init_packet_length);
#ifdef VERBOSE
        get_dec_str(decText, 7, err_code);
        WRITE_VERBOSE("prevalidate=", 12);
        WRITE_VERBOSE(decText, 8);
        WRITE_VERBOSE("\r\n", 3);
#endif
        if (err_code == NRF_SUCCESS)
        {
            m_dfu_state = DFU_STATE_RX_DATA_PKT;
        }
        else
        {
            m_init_packet_length = 0;
        }
    }
    return err_code;
}


uint32_t dfu_init_pkt_handle(dfu_update_packet_t * p_packet)
{
    uint32_t err_code = NRF_SUCCESS;
    uint32_t length;

#ifdef VERBOSE
WRITE_VERBOSE("init_pkt_handle\r\n", 18);
char decText[8] = {0};
get_dec_str(decText, 7, m_dfu_state);
WRITE_VERBOSE("state = ", 9);
WRITE_VERBOSE(decText, 8);
WRITE_VERBOSE("\r\n", 3);
#endif

    switch (m_dfu_state)
    {
        case DFU_STATE_RDY:
            m_dfu_state = DFU_STATE_RX_INIT_PKT;
            // When receiving init packet in state ready just update and fall through this case.

        case DFU_STATE_RX_INIT_PKT:
            // DFU initialization has been done and a start packet has been received.
            if (IMAGE_WRITE_IN_PROGRESS())
            {
                // Image write is already in progress. Cannot handle an init packet now.
                return NRF_ERROR_INVALID_STATE;
            }

            // Valid peer activity detected. Hence restart the DFU timer.
            err_code = dfu_timer_restart();
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }

            length = p_packet->params.data_packet.packet_length * sizeof(uint32_t);
            if ((m_init_packet_length + length) > sizeof(m_init_packet))
            {
                return NRF_ERROR_INVALID_LENGTH;
            }

            memcpy(&m_init_packet[m_init_packet_length],
                   &p_packet->params.data_packet.p_data_packet[0],
                   length);
            m_init_packet_length += length;
            break;

        default:
            // Either the start packet was not received or dfu_init function was not called before.
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }

    return err_code;
}


uint32_t dfu_image_validate()
{
    uint32_t err_code;

#ifdef VERBOSE
    char decText[8] = {0};
    get_dec_str(decText, 7, m_dfu_state);
    WRITE_VERBOSE("state = ", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, m_data_received);
    WRITE_VERBOSE("dat rec=", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);

    get_dec_str(decText, 7, m_image_size);
    WRITE_VERBOSE("img len=", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
#endif

    switch (m_dfu_state)
    {
        case DFU_STATE_RX_DATA_PKT:
            // Check if the application image write has finished.
            if (m_data_received != m_image_size)
            {
                // Image not yet fully transfered by the peer or the peer has attempted to write
                // too much data. Hence the validation should fail.
                err_code = NRF_ERROR_INVALID_STATE;
            }
            else
            {
                m_dfu_state = DFU_STATE_VALIDATE;

                // Valid peer activity detected. Hence restart the DFU timer.
WRITE_VERBOSE("dfu_timer_re\r\n", 14);
                err_code = dfu_timer_restart();
                if (err_code == NRF_SUCCESS)
                {
WRITE_VERBOSE("postvalidate\r\n", 14);
                    err_code = dfu_init_postvalidate((uint8_t *)mp_storage_handle_active->block_id,
                                                     m_image_size);
                    if (err_code != NRF_SUCCESS)
                    {
                        return err_code;
                    }

                    m_dfu_state = DFU_STATE_WAIT_4_ACTIVATE;
                }
            }
            break;

        default:
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }

    return err_code;
}


uint32_t dfu_image_activate()
{
#ifdef VERBOSE
    WRITE_VERBOSE("img activate\r\n", 15);
    char decText[8] = {0};
    get_dec_str(decText, 7, m_dfu_state);
    WRITE_VERBOSE("state = ", 8);
    WRITE_VERBOSE(decText, 8);
    WRITE_VERBOSE("\r\n", 3);
#endif

    uint32_t err_code;

    switch (m_dfu_state)
    {
        case DFU_STATE_WAIT_4_ACTIVATE:

            // Stop the DFU Timer because the peer activity need not be monitored any longer.
            err_code = app_timer_stop(m_dfu_timer_id);
            APP_ERROR_CHECK(err_code);

            err_code = m_functions.activate();
            break;

        default:
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }

    return err_code;
}


void dfu_reset(void)
{
WRITE_VERBOSE("dfu_reset\r\n", 12);
    dfu_update_status_t update_status;

    update_status.status_code = DFU_RESET;

    bootloader_dfu_update_process(update_status);
}


static uint32_t dfu_compare_block(uint32_t * ptr1, uint32_t * ptr2, uint32_t len)
{
    sd_mbr_command_t sd_mbr_cmd;

    sd_mbr_cmd.command             = SD_MBR_COMMAND_COMPARE;
    sd_mbr_cmd.params.compare.ptr1 = ptr1;
    sd_mbr_cmd.params.compare.ptr2 = ptr2;
    sd_mbr_cmd.params.compare.len  = len / sizeof(uint32_t);

    return sd_mbr_command(&sd_mbr_cmd);
}


static uint32_t dfu_copy_sd(uint32_t * src, uint32_t * dst, uint32_t len)
{
    sd_mbr_command_t sd_mbr_cmd;

    sd_mbr_cmd.command            = SD_MBR_COMMAND_COPY_SD;
    sd_mbr_cmd.params.copy_sd.src = src;
    sd_mbr_cmd.params.copy_sd.dst = dst;
    sd_mbr_cmd.params.copy_sd.len = len / sizeof(uint32_t);

    return sd_mbr_command(&sd_mbr_cmd);
}


static uint32_t dfu_sd_img_block_swap(uint32_t * src,
                                      uint32_t * dst,
                                      uint32_t len,
                                      uint32_t block_size)
{
    // It is neccesarry to swap the new SoftDevice in 3 rounds to ensure correct copy of data
    // and verifucation of data in case power reset occurs during write to flash.
    // To ensure the robustness of swapping the images are compared backwards till start of
    // image swap. If the back is identical everything is swapped.
    uint32_t err_code = dfu_compare_block(src, dst, len);
    if (err_code == NRF_SUCCESS)
    {
        return err_code;
    }

    if ((uint32_t)dst > SOFTDEVICE_REGION_START)
    {
        err_code = dfu_sd_img_block_swap((uint32_t *)((uint32_t)src - block_size),
                                         (uint32_t *)((uint32_t)dst - block_size),
                                         block_size,
                                         block_size);
        if (err_code != NRF_SUCCESS)
        {
            return err_code;
        }
    }

    err_code = dfu_copy_sd(src, dst, len);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }
    return dfu_compare_block(src, dst, len);
}


uint32_t dfu_sd_image_swap(void)
{
    bootloader_settings_t boot_settings;

    bootloader_settings_get(&boot_settings);

    if (boot_settings.sd_image_size == 0)
    {
        return NRF_SUCCESS;
    }

    if ((SOFTDEVICE_REGION_START + boot_settings.sd_image_size) > boot_settings.sd_image_start)
    {
        uint32_t err_code;
        uint32_t sd_start        = SOFTDEVICE_REGION_START;
        uint32_t block_size      = (boot_settings.sd_image_start - sd_start) / 2;
        uint32_t image_end       = boot_settings.sd_image_start + boot_settings.sd_image_size;

        uint32_t img_block_start = boot_settings.sd_image_start + 2 * block_size;
        uint32_t sd_block_start  = sd_start + 2 * block_size;

        if (SD_SIZE_GET(MBR_SIZE) < boot_settings.sd_image_size)
        {
            // This will clear a page thus ensuring the old image is invalidated before swapping.
            err_code = dfu_copy_sd((uint32_t *)(sd_start + block_size),
                                   (uint32_t *)(sd_start + block_size),
                                   sizeof(uint32_t));
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }

            err_code = dfu_copy_sd((uint32_t *)sd_start, (uint32_t *)sd_start, sizeof(uint32_t));
            if (err_code != NRF_SUCCESS)
            {
                return err_code;
            }
        }

        return dfu_sd_img_block_swap((uint32_t *)img_block_start,
                                     (uint32_t *)sd_block_start,
                                     image_end - img_block_start,
                                     block_size);
    }
    else
    {
        if (boot_settings.sd_image_size != 0)
        {
            return dfu_copy_sd((uint32_t *)boot_settings.sd_image_start,
                               (uint32_t *)SOFTDEVICE_REGION_START,
                               boot_settings.sd_image_size);
        }
    }

    return NRF_SUCCESS;
}


uint32_t dfu_bl_image_swap(void)
{
    bootloader_settings_t bootloader_settings;
    sd_mbr_command_t      sd_mbr_cmd;

    bootloader_settings_get(&bootloader_settings);

    if (bootloader_settings.bl_image_size != 0)
    {
        uint32_t bl_image_start = (bootloader_settings.sd_image_size == 0) ?
                                  DFU_BANK_1_REGION_START :
                                  bootloader_settings.sd_image_start +
                                  bootloader_settings.sd_image_size;

        sd_mbr_cmd.command               = SD_MBR_COMMAND_COPY_BL;
        sd_mbr_cmd.params.copy_bl.bl_src = (uint32_t *)(bl_image_start);
        sd_mbr_cmd.params.copy_bl.bl_len = bootloader_settings.bl_image_size / sizeof(uint32_t);

        return sd_mbr_command(&sd_mbr_cmd);
    }
    return NRF_SUCCESS;
}


uint32_t dfu_bl_image_validate(void)
{
    bootloader_settings_t bootloader_settings;
    sd_mbr_command_t      sd_mbr_cmd;

    bootloader_settings_get(&bootloader_settings);

    if (bootloader_settings.bl_image_size != 0)
    {
        uint32_t bl_image_start = (bootloader_settings.sd_image_size == 0) ?
                                  DFU_BANK_1_REGION_START :
                                  bootloader_settings.sd_image_start +
                                  bootloader_settings.sd_image_size;

        sd_mbr_cmd.command             = SD_MBR_COMMAND_COMPARE;
        sd_mbr_cmd.params.compare.ptr1 = (uint32_t *)BOOTLOADER_REGION_START;
        sd_mbr_cmd.params.compare.ptr2 = (uint32_t *)(bl_image_start);
        sd_mbr_cmd.params.compare.len  = bootloader_settings.bl_image_size / sizeof(uint32_t);

        uint32_t err_code;
        err_code = sd_mbr_command(&sd_mbr_cmd);

        // [3.2.17] The APP_RESERVED_DATA value changed between bootloader version 1.0.0 and 1.1.0
        //  because of this change the DFU_BANK_1_REGION_START changes, and uploading a bootloader
        //  from version 1.0.0 to 1.1.0 will brick the bootloader, to avoid this, also check the
        //  DFU_BANK_1_REGION_START_V1_0_0 when validating a bootloader.
        //  See comments on nordic forum here:
        //    https://devzone.nordicsemi.com/question/54242/updating-bootloader-to-bootloader-which-preserves-app-data/
        //    https://devzone.nordicsemi.com/question/81141/cant-update-bootlader-over-the-air-when-changing-dfu_app_data_reserved/
        if (err_code != NRF_SUCCESS) {
            uint32_t bl_image_start = (bootloader_settings.sd_image_size == 0) ?
                                      DFU_BANK_1_REGION_START_V1_0_0 :
                                      bootloader_settings.sd_image_start +
                                      bootloader_settings.sd_image_size;

            sd_mbr_cmd.params.compare.ptr2 = (uint32_t *)(bl_image_start);

            err_code = sd_mbr_command(&sd_mbr_cmd);

            // todo: continue here checking other versions if not successful
        }

		return err_code;
    }
    return NRF_SUCCESS;
}


uint32_t dfu_sd_image_validate(void)
{
    bootloader_settings_t bootloader_settings;
    sd_mbr_command_t      sd_mbr_cmd;

    bootloader_settings_get(&bootloader_settings);

    if (bootloader_settings.sd_image_size == 0)
    {
        return NRF_SUCCESS;
    }

    if ((SOFTDEVICE_REGION_START + bootloader_settings.sd_image_size) > bootloader_settings.sd_image_start)
    {
        uint32_t sd_start        = SOFTDEVICE_REGION_START;
        uint32_t block_size      = (bootloader_settings.sd_image_start - sd_start) / 2;
        uint32_t image_end       = bootloader_settings.sd_image_start +
                                   bootloader_settings.sd_image_size;

        uint32_t img_block_start = bootloader_settings.sd_image_start + 2 * block_size;
        uint32_t sd_block_start  = sd_start + 2 * block_size;

        if (SD_SIZE_GET(MBR_SIZE) < bootloader_settings.sd_image_size)
        {
            return NRF_ERROR_NULL;
        }

        return dfu_sd_img_block_swap((uint32_t *)img_block_start,
                                     (uint32_t *)sd_block_start,
                                     image_end - img_block_start,
                                     block_size);
    }

    sd_mbr_cmd.command             = SD_MBR_COMMAND_COMPARE;
    sd_mbr_cmd.params.compare.ptr1 = (uint32_t *)SOFTDEVICE_REGION_START;
    sd_mbr_cmd.params.compare.ptr2 = (uint32_t *)bootloader_settings.sd_image_start;
    sd_mbr_cmd.params.compare.len  = bootloader_settings.sd_image_size / sizeof(uint32_t);

    return sd_mbr_command(&sd_mbr_cmd);
}
