/*
 * Copyright 2020 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file
 * @brief Implementation of the port UART API for the Zephyr platform.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include <zephyr/types.h>
#include <zephyr.h>
#include <drivers/uart.h>

#include <device.h>
#include <soc.h>

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "stdio.h"

#include "u_cfg_sw.h"
#include "u_cfg_hw_platform_specific.h"
#include "u_error_common.h"
#include "u_port_debug.h"
#include "u_port.h"
#include "u_port_os.h"
#include "u_port_event_queue.h"
#include "u_port_uart.h"

#include "string.h" // For memcpy()

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef U_PORT_UART_MAX_NUM
/** The number of UARTs that are available on the NRF53 chip.
 * There are up to 4 UART HW blocks available, how many are
 * connected depends on the chip revision.
 */
#define U_PORT_UART_MAX_NUM 2
#endif

#ifndef U_PORT_UART_BUFFER_SIZE
/** The UART buffer size for receive in bytes.
 */
#define U_PORT_UART_BUFFER_SIZE 4096
#endif

/**
 * @brief This is temporary
 * 
 * @todo move this to somewhere else
 * 
 */
#define LTE_UART    DT_ALIAS(lteuart)
/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Structure of the things we need to keep track of per UART.
 */
typedef struct {
    const struct device *pDevice;
    struct uart_config config;
    int32_t eventQueueHandle;
    uint32_t eventFilter;
    void (*pEventCallback)(int32_t, uint32_t, void *);
    void *pEventCallbackParam;
    char *pBuffer;
    uint32_t bufferRead;
    int32_t bufferWrite;
    bool bufferFull;
    struct k_timer rxTimer;
    struct uartData_t *pTxData;
    struct k_fifo fifoTxData;
    uint32_t txWritten;
    struct k_sem txSem;
} uPortUartData_t;

/** Structure describing an event.
 */
typedef struct {
    int32_t uartHandle;
    uint32_t eventBitMap;
} uPortUartEvent_t;

struct uartData_t {
    int32_t handle;
    char *pData;
    uint16_t len;
} uartData_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// Mutex to protect UART data.
static uPortMutexHandle_t gMutex = NULL;
static uPortUartData_t gUartData[U_PORT_UART_MAX_NUM];

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Event handler, calls the user's event callback.
static void eventHandler(void *pParam, size_t paramLength)
{
    uPortUartEvent_t *pEvent = (uPortUartEvent_t *) pParam;

    (void) paramLength;

    // Don't need to worry about locking the mutex,
    // the close() function makes sure this event handler
    // exits cleanly and, in any case, the user callback
    // will want to be able to access functions in this
    // API which will need to lock the mutex.

    if ((pEvent->uartHandle >= 0) &&
        (pEvent->uartHandle < sizeof(gUartData) / sizeof(gUartData[0]))) {
        if (gUartData[pEvent->uartHandle].pEventCallback != NULL) {
            gUartData[pEvent->uartHandle].pEventCallback(pEvent->uartHandle,
                                                         pEvent->eventBitMap,
                                                         gUartData[pEvent->uartHandle].pEventCallbackParam);
        }
    }
}

// Close a UART instance
// Note: gMutex should be locked before this is called.
static void uartClose(int32_t handle)
{
    k_free(gUartData[handle].pBuffer);
    gUartData[handle].pBuffer = NULL;
    uart_irq_rx_disable(gUartData[handle].pDevice);
    uart_irq_tx_disable(gUartData[handle].pDevice);

    gUartData[handle].bufferRead = 0;
    gUartData[handle].bufferWrite = 0;
    gUartData[handle].bufferFull = false;
    gUartData[handle].eventQueueHandle = -1;
    gUartData[handle].eventFilter = 0;
    gUartData[handle].pEventCallback = NULL;
    gUartData[handle].pEventCallbackParam = NULL;
    gUartData[handle].pTxData = NULL;
    gUartData[handle].txWritten = 0;
}

static void rxTimer(struct k_timer *timer_id)
{
    uint32_t uart = (uint32_t)(timer_id->user_data);

    if ((gUartData[uart].eventQueueHandle >= 0) &&
        (gUartData[uart].eventFilter & U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED)) {
        uPortUartEvent_t event;
        event.uartHandle = uart;
        event.eventBitMap = U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED;
        uPortEventQueueSendIrq(gUartData[uart].eventQueueHandle,
                               &event, sizeof(event));
    }
}

static void uartCb(const struct device *uart, void *user_data)
{
    uint8_t i;

    for (i = 0; i < U_PORT_UART_MAX_NUM; i++) {
        if (uart == gUartData[i].pDevice) {
            break;
        }
    }

    if (i == U_PORT_UART_MAX_NUM) {
        return;
    }

    uart_irq_update(uart);

    if (uart_irq_rx_ready(uart)) {
        bool read = false;
        if (!gUartData[i].bufferFull) {
            while (uart_fifo_read(uart, (gUartData[i].pBuffer + gUartData[i].bufferWrite), 1) != 0) {
                gUartData[i].bufferWrite++;
                gUartData[i].bufferWrite %= U_PORT_UART_BUFFER_SIZE;
                read = true;

                if (gUartData[i].bufferWrite == gUartData[i].bufferRead) {
                    gUartData[i].bufferFull = true;
                    uart_irq_rx_disable(uart);
                    k_timer_stop(&gUartData[i].rxTimer);
                    if ((gUartData[i].eventQueueHandle >= 0) &&
                        (gUartData[i].eventFilter & U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED)) {
                        uPortUartEvent_t event;
                        event.uartHandle = i;
                        event.eventBitMap = U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED;
                        uPortEventQueueSendIrq(gUartData[i].eventQueueHandle,
                                               &event, sizeof(event));
                    }
                    break;
                } else {

                }

            }

            if (read) {
                k_timer_start(&gUartData[i].rxTimer, K_MSEC(1), K_NO_WAIT);
            }
        }
    }

    if (uart_irq_tx_ready(uart)) {
        if (gUartData[i].pTxData == NULL) {
            gUartData[i].pTxData = k_fifo_get(&gUartData[i].fifoTxData, K_NO_WAIT);
            gUartData[i].txWritten = 0;
            // uart_irq_tx_disable(uart);
        }

        if (!gUartData[i].pTxData) {
            uart_irq_tx_disable(uart);
            return;
        }

        if (gUartData[i].pTxData->len > gUartData[i].txWritten) {

            gUartData[i].txWritten += uart_fifo_fill(uart,
                                                     gUartData[i].pTxData->pData + gUartData[i].txWritten,
                                                     gUartData[i].pTxData->len - gUartData[i].txWritten);

        } else {
            gUartData[i].pTxData = NULL;
            gUartData[i].txWritten = 0;
            k_sem_give(&gUartData[i].txSem);

            if (k_fifo_is_empty(&gUartData[i].fifoTxData)) {
                uart_irq_tx_disable(uart);
            }
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

int32_t uPortUartInit()
{
    uErrorCode_t errorCode = U_ERROR_COMMON_SUCCESS;

    if (gMutex == NULL) {
        errorCode = uPortMutexCreate(&gMutex);
        for (size_t x = 0; x < sizeof(gUartData) / sizeof(gUartData[0]); x++) {
            const struct device *dev = NULL;
            switch (x) {
                case 0:
                    /**
                     * @brief this needs modification. 
                     * 
                     * @todo Do cleanup here.
                     */
                    dev = device_get_binding(DT_LABEL(LTE_UART));
                    // dev = device_get_binding("UART_0");
                    break;
                case 1:
                    dev = device_get_binding("UART_1");
                    break;
                case 2:
                    dev = device_get_binding("UART_2");
                    break;
                case 3:
                    dev = device_get_binding("UART_3");
                    break;
                default:
                    break;
            }

            gUartData[x].pDevice = dev;
            gUartData[x].pBuffer = NULL;
        }
    }

    return (int32_t) errorCode;
}

void uPortUartDeinit()
{
    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);
        for (size_t x = 0; x < sizeof(gUartData) / sizeof(gUartData[0]); x++) {
            if (gUartData[x].pDevice != NULL) {
                uartClose(x);
                gUartData[x].pDevice = NULL;
            }
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
        uPortMutexDelete(gMutex);
        gMutex = NULL;
    }
}

int32_t uPortUartOpen(int32_t uart, int32_t baudRate,
                      void *pReceiveBuffer,
                      size_t receiveBufferSizeBytes,
                      int32_t pinTx, int32_t pinRx,
                      int32_t pinCts, int32_t pinRts)
{
    uErrorCode_t handleOrErrorCode = U_ERROR_COMMON_NOT_INITIALISED;

    // Note that the pins passed into this function must be set
    // to -1 since the Zephyr platform used on NRF53 does not
    // permit the pin assignments to be set at run-time, only at
    // compile-time.  To obtain the real values for your peripheral
    // pin assignments take a look at the macros U_CFG_TEST_PIN_UART_A_xxx_GET
    // (e.g. U_CFG_TEST_PIN_UART_A_TXD_GET) in the file
    // `u_cfg_test_plaform_specific.h` for this platform, which
    // demonstrate a mechanism for doing this.

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        handleOrErrorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((uart >= 0) &&
            (uart < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[uart].pDevice != NULL) &&
            (pReceiveBuffer == NULL) &&
            (gUartData[uart].pBuffer == NULL) &&
            (pinTx < 0) && (pinRx < 0) && (pinCts < 0) && (pinRts < 0)) {

            gUartData[uart].pBuffer = k_malloc(U_PORT_UART_BUFFER_SIZE);

            if (gUartData[uart].pBuffer == NULL) {
                handleOrErrorCode = U_ERROR_COMMON_NO_MEMORY;
            } else {
                k_sem_init(&gUartData[uart].txSem, 0, 1);

                gUartData[uart].bufferRead = 0;
                gUartData[uart].bufferWrite = 0;
                gUartData[uart].bufferFull = false;
                gUartData[uart].eventQueueHandle = -1;
                gUartData[uart].eventFilter = 0;
                gUartData[uart].pEventCallback = NULL;
                gUartData[uart].pEventCallbackParam = NULL;
                k_timer_init(&gUartData[uart].rxTimer, rxTimer, NULL);
                k_timer_user_data_set(&gUartData[uart].rxTimer, (void *)uart);
                k_fifo_init(&gUartData[uart].fifoTxData);
                gUartData[uart].pTxData = NULL;
                gUartData[uart].txWritten = 0;

                uart_config_get(gUartData[uart].pDevice, &gUartData[uart].config);
                // Flow control is set in the .overlay file
                // by including the line:
                //     hw-flow-control;
                // in the definition of the relevant UART
                // so all we need to configure here is the
                // baud rate as everything else is good at the
                // default values (8N1).
                gUartData[uart].config.baudrate = baudRate;
                gUartData[uart].config.parity = UART_CFG_PARITY_NONE;
                gUartData[uart].config.stop_bits = UART_CFG_STOP_BITS_1;
                gUartData[uart].config.data_bits = UART_CFG_DATA_BITS_8;
                uart_configure(gUartData[uart].pDevice, &gUartData[uart].config);
                uart_irq_callback_user_data_set(gUartData[uart].pDevice, uartCb, NULL);
                uart_irq_rx_enable(gUartData[uart].pDevice);
                uart_irq_tx_enable(gUartData[uart].pDevice);
                handleOrErrorCode = uart;
            }
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return (int32_t) handleOrErrorCode;
}

void uPortUartClose(int32_t handle)
{
    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL)) {
            uartClose(handle);
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }
}

int32_t uPortUartGetReceiveSize(int32_t handle)
{
    uErrorCode_t sizeOrErrorCode = U_ERROR_COMMON_NOT_INITIALISED;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        sizeOrErrorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL)) {

            if (gUartData[handle].bufferFull) {
                sizeOrErrorCode = U_PORT_UART_BUFFER_SIZE;
            } else {
                sizeOrErrorCode = gUartData[handle].bufferWrite - gUartData[handle].bufferRead;

                if (sizeOrErrorCode < 0) {
                    sizeOrErrorCode = (U_PORT_UART_BUFFER_SIZE - gUartData[handle].bufferRead) +
                                      gUartData[handle].bufferWrite;
                }
            }
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return (int32_t) sizeOrErrorCode;
}

int32_t uPortUartRead(int32_t handle, void *pBuffer,
                      size_t sizeBytes)
{
    uErrorCode_t sizeOrErrorCode = U_ERROR_COMMON_NOT_INITIALISED;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        sizeOrErrorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pBuffer != NULL) && (sizeBytes > 0) && (handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL)) {

            if (gUartData[handle].bufferWrite == gUartData[handle].bufferRead &&
                gUartData[handle].bufferFull == false) {
                sizeOrErrorCode = 0;
            } else {
                int32_t toRead = 0;
                if (gUartData[handle].bufferFull) {
                    sizeOrErrorCode = U_PORT_UART_BUFFER_SIZE;
                } else {
                    sizeOrErrorCode = gUartData[handle].bufferWrite - gUartData[handle].bufferRead;

                    if (sizeOrErrorCode == 0) {
                        sizeOrErrorCode = U_PORT_UART_BUFFER_SIZE;
                    } else if (sizeOrErrorCode < 0) {
                        sizeOrErrorCode = (U_PORT_UART_BUFFER_SIZE - gUartData[handle].bufferRead) +
                                          gUartData[handle].bufferWrite;
                    }
                }

                if (sizeOrErrorCode > sizeBytes) {
                    sizeOrErrorCode = sizeBytes;
                }

                toRead = sizeOrErrorCode;

                uint32_t read = MIN(toRead, U_PORT_UART_BUFFER_SIZE - gUartData[handle].bufferRead);
                memcpy(pBuffer, gUartData[handle].pBuffer + gUartData[handle].bufferRead, read);
                toRead -= read;
                gUartData[handle].bufferRead += read;
                gUartData[handle].bufferRead %= U_PORT_UART_BUFFER_SIZE;

                if (toRead > 0) {
                    memcpy((char *)pBuffer + read, gUartData[handle].pBuffer, toRead);
                    gUartData[handle].bufferRead += toRead;
                }

                gUartData[handle].bufferFull = false;
                uart_irq_rx_enable(gUartData[handle].pDevice);
            }
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return (int32_t) sizeOrErrorCode;
}

int32_t uPortUartWrite(int32_t handle, const void *pBuffer,
                       size_t sizeBytes)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_NOT_INITIALISED;

    if (gMutex != NULL) {
        errorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pBuffer != NULL) && (sizeBytes > 0) && (handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL)) {
            errorCode = U_ERROR_COMMON_NOT_INITIALISED;

            U_PORT_MUTEX_LOCK(gMutex);

            struct uartData_t data;
            data.handle = handle;
            data.pData = (void *)pBuffer;
            data.len = sizeBytes;

            // Hint when debugging: if your code stops dead here
            // it is because the CTS line of this MCU's UART HW
            // is floating high, stopping the UART from
            // transmitting once its buffer is full: either
            // the thing at the other end doesn't want data sent to
            // it or the CTS pin when configuring this UART
            // was wrong and it's not connected to the right
            // thing.
            k_fifo_put(&gUartData[handle].fifoTxData, &data);
            uart_irq_tx_enable(gUartData[handle].pDevice);
            // UART write is async to wait here to make this function syncronous
            k_sem_take(&gUartData[handle].txSem, K_FOREVER);
            U_PORT_MUTEX_UNLOCK(gMutex);

            errorCode = sizeBytes;
        }
    }

    return (int32_t) errorCode;
}

int32_t uPortUartEventCallbackSet(int32_t handle,
                                  uint32_t filter,
                                  void (*pFunction)(int32_t,
                                                    uint32_t,
                                                    void *),
                                  void *pParam,
                                  size_t stackSizeBytes,
                                  int32_t priority)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_NOT_INITIALISED;
    char name[16];

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        errorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle < 0) &&
            (filter != 0) && (pFunction != NULL)) {
            // Open an event queue to eventHandler()
            // which will receive uPortUartEvent_t
            // and give it a useful name for debug purposes
            snprintf(name, sizeof(name), "eventUart_%d", (int) handle);
            errorCode = uPortEventQueueOpen(eventHandler, name,
                                            sizeof(uPortUartEvent_t),
                                            stackSizeBytes,
                                            priority,
                                            U_PORT_UART_EVENT_QUEUE_SIZE);
            if (errorCode >= 0) {
                gUartData[handle].eventQueueHandle = (int32_t) errorCode;
                gUartData[handle].eventQueueHandle = (int32_t) errorCode;
                gUartData[handle].pEventCallback = pFunction;
                gUartData[handle].pEventCallbackParam = pParam;
                gUartData[handle].eventFilter = filter;
                errorCode = U_ERROR_COMMON_SUCCESS;
            }
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return errorCode;
}

void uPortUartEventCallbackRemove(int32_t handle)
{
    int32_t eventQueueHandle = -1;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0)) {
            // Save the eventQueueHandle and set all
            // the parameters to indicate that the
            // queue is closed
            eventQueueHandle = gUartData[handle].eventQueueHandle;
            gUartData[handle].eventQueueHandle = -1;
            gUartData[handle].pEventCallback = NULL;
            gUartData[handle].eventFilter = 0;
        }

        U_PORT_MUTEX_UNLOCK(gMutex);

        // Now close the event queue
        // outside the gMutex lock.  Reason for this
        // is that the event task could be calling
        // back into here and we don't want it
        // blocked by us or we'll get stuck.
        if (eventQueueHandle >= 0) {
            uPortEventQueueClose(eventQueueHandle);
        }
    }
}

uint32_t uPortUartEventCallbackFilterGet(int32_t handle)
{
    uint32_t filter = 0;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0)) {
            filter = gUartData[handle].eventFilter;
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return filter;
}

int32_t uPortUartEventCallbackFilterSet(int32_t handle,
                                        uint32_t filter)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_NOT_INITIALISED;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        errorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0) &&
            (filter != 0)) {
            gUartData[handle].eventFilter = filter;
            errorCode = U_ERROR_COMMON_SUCCESS;
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return errorCode;
}

int32_t uPortUartEventSend(int32_t handle, uint32_t eventBitMap)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_NOT_INITIALISED;
    uPortUartEvent_t event;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        errorCode = U_ERROR_COMMON_INVALID_PARAMETER;
        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0) &&
            // The only event we support right now
            (eventBitMap == U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED)) {
            event.uartHandle = handle;
            event.eventBitMap = eventBitMap;
            errorCode = uPortEventQueueSend(gUartData[handle].eventQueueHandle,
                                            &event, sizeof(event));
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return errorCode;
}

bool uPortUartEventIsCallback(int32_t handle)
{
    bool isEventCallback = false;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0)) {
            isEventCallback = uPortEventQueueIsTask(gUartData[handle].eventQueueHandle);
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return isEventCallback;
}

int32_t uPortUartEventStackMinFree(int32_t handle)
{
    int32_t sizeOrErrorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;

    if (gMutex != NULL) {

        U_PORT_MUTEX_LOCK(gMutex);

        sizeOrErrorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((handle >= 0) &&
            (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
            (gUartData[handle].pDevice != NULL) &&
            (gUartData[handle].eventQueueHandle >= 0)) {
            sizeOrErrorCode = uPortEventQueueStackMinFree(gUartData[handle].eventQueueHandle);
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return sizeOrErrorCode;
}

bool uPortUartIsRtsFlowControlEnabled(int32_t handle)
{
    bool rtsFlowControlIsEnabled = false;

    if ((gMutex != NULL) && (handle >= 0) &&
        (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
        (gUartData[handle].pDevice != NULL)) {

        U_PORT_MUTEX_LOCK(gMutex);
        if (gUartData[handle].config.flow_ctrl == UART_CFG_FLOW_CTRL_RTS_CTS) {
            rtsFlowControlIsEnabled = true;
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return rtsFlowControlIsEnabled;
}

bool uPortUartIsCtsFlowControlEnabled(int32_t handle)
{
    bool ctsFlowControlIsEnabled = false;

    if ((gMutex != NULL) && (handle >= 0) &&
        (handle < sizeof(gUartData) / sizeof(gUartData[0])) &&
        (gUartData[handle].pDevice != NULL)) {

        U_PORT_MUTEX_LOCK(gMutex);

        if (gUartData[handle].config.flow_ctrl == UART_CFG_FLOW_CTRL_RTS_CTS) {
            ctsFlowControlIsEnabled = true;
        }

        U_PORT_MUTEX_UNLOCK(gMutex);
    }

    return ctsFlowControlIsEnabled;
}

// End of file
