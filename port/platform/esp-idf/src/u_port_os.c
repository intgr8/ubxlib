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
 * @brief Implementation of the port OS API for the ESP32 platform.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

/* The remaining include files come after the mutex debug macros. */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS FOR MUTEX DEBUG
 * -------------------------------------------------------------- */

#ifdef U_CFG_MUTEX_DEBUG
/** If we're adding the mutex debug intermediate functions to
 * the build then the implementations of the mutex functions
 * here get an underscore before them
 */
# define MAKE_MTX_FN(x, ...) _ ## x __VA_OPT__(,) __VA_ARGS__
#else
/** The normal case: a mutex function is not fiddled with.
 */
# define MAKE_MTX_FN(x, ...) x __VA_OPT__(,) __VA_ARGS__
#endif

/** This macro, working in conjunction with the MAKE_MTX_FN()
 * macro above, should wrap all of the uPortOsMutex* functions
 * in this file.  The functions are then pre-fixed with an
 * underscore if U_CFG_MUTEX_DEBUG is defined, allowing the
 * intermediate mutex macros/functions over in u_mutex_debug.c
 * to take their place.  Those functions subsequently call
 * back into the "underscore versions" of the uPortOsMutex*
 * functions here.
 */
#define MTX_FN(x, ...) MAKE_MTX_FN(x __VA_OPT__(,) __VA_ARGS__)

// Now undef U_CFG_MUTEX_DEBUG so that this file is not polluted
// by the u_mutex_debug.h stuff brought in through u_port_os.h.
#undef U_CFG_MUTEX_DEBUG

/* ----------------------------------------------------------------
 * INCLUDE FILES
 * -------------------------------------------------------------- */

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"

#include "u_cfg_sw.h"
#include "u_cfg_os_platform_specific.h"
#include "u_error_common.h"
#include "u_port_debug.h"
#include "u_port.h"
#include "u_port_os.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: TASKS
 * -------------------------------------------------------------- */

// Create a task.
int32_t uPortTaskCreate(void (*pFunction)(void *),
                        const char *pName,
                        size_t stackSizeBytes,
                        void *pParameter,
                        int32_t priority,
                        uPortTaskHandle_t *pTaskHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((pFunction != NULL) && (pTaskHandle != NULL) &&
        (priority >= U_CFG_OS_PRIORITY_MIN) &&
        (priority <= U_CFG_OS_PRIORITY_MAX)) {
        if (xTaskCreate(pFunction, pName, stackSizeBytes,
                        pParameter, priority,
                        (TaskHandle_t *) pTaskHandle) == pdPASS) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Delete the given task.
int32_t uPortTaskDelete(const uPortTaskHandle_t taskHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    // Can only delete oneself in freeRTOS
    if (taskHandle == NULL) {
        vTaskDelete((TaskHandle_t) taskHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

// Check if the current task handle is equal to the given task handle.
bool uPortTaskIsThis(const uPortTaskHandle_t taskHandle)
{
    return xTaskGetCurrentTaskHandle() == (TaskHandle_t) taskHandle;
}

// Block the current task for a time.
void uPortTaskBlock(int32_t delayMs)
{
    vTaskDelay(delayMs / portTICK_PERIOD_MS);
}

// Get the minimum free stack for a given task.
int32_t uPortTaskStackMinFree(const uPortTaskHandle_t taskHandle)
{
    TaskHandle_t handle = (TaskHandle_t) taskHandle;

    if (handle == NULL) {
        handle = xTaskGetCurrentTaskHandle();
    }

    // On ESP32 the water mark is returned in bytes rather
    // than words so this can just be returned directly
    return uxTaskGetStackHighWaterMark(handle);
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: QUEUES
 * -------------------------------------------------------------- */

// Create a queue.
int32_t uPortQueueCreate(size_t queueLength,
                         size_t itemSizeBytes,
                         uPortQueueHandle_t *pQueueHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (pQueueHandle != NULL) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        // Actually create the queue
        *pQueueHandle = (uPortQueueHandle_t) xQueueCreate(queueLength,
                                                          itemSizeBytes);
        if (*pQueueHandle != NULL) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Delete the given queue.
int32_t uPortQueueDelete(const uPortQueueHandle_t queueHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (queueHandle != NULL) {
        vQueueDelete((QueueHandle_t) queueHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

// Send to the given queue.
int32_t uPortQueueSend(const uPortQueueHandle_t queueHandle,
                       const void *pEventData)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((queueHandle != NULL) && (pEventData != NULL)) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xQueueSend((QueueHandle_t) queueHandle,
                       pEventData,
                       (portTickType) portMAX_DELAY) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Send to the given queue from an interrupt.
int32_t uPortQueueSendIrq(const uPortQueueHandle_t queueHandle,
                          const void *pEventData)
{
    BaseType_t yield = false;
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((queueHandle != NULL) && (pEventData != NULL)) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xQueueSendFromISR((QueueHandle_t) queueHandle,
                              pEventData,
                              &yield) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    // Required for correct FreeRTOS operation
    if (yield) {
        taskYIELD();
    }

    return (int32_t) errorCode;
}

// Receive from the given queue, blocking.
int32_t uPortQueueReceive(const uPortQueueHandle_t queueHandle,
                          void *pEventData)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((queueHandle != NULL) && (pEventData != NULL)) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xQueueReceive((QueueHandle_t) queueHandle,
                          pEventData,
                          (portTickType) portMAX_DELAY) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Receive from the given queue, with a wait time.
int32_t uPortQueueTryReceive(const uPortQueueHandle_t queueHandle,
                             int32_t waitMs, void *pEventData)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((queueHandle != NULL) && (pEventData != NULL)) {
        errorCode = U_ERROR_COMMON_TIMEOUT;
        if (xQueueReceive((QueueHandle_t) queueHandle,
                          pEventData,
                          waitMs / portTICK_PERIOD_MS) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Peek the given queue.
int32_t uPortQueuePeek(const uPortQueueHandle_t queueHandle,
                       void *pEventData)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((queueHandle != NULL) && (pEventData != NULL)) {
        errorCode = U_ERROR_COMMON_TIMEOUT;
        if (xQueuePeek((QueueHandle_t) queueHandle,
                       pEventData,
                       (portTickType) portMAX_DELAY) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Get the number of free spaces in the given queue.
int32_t uPortQueueGetFree(const uPortQueueHandle_t queueHandle)
{
    int32_t errorCodeOrFree = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;

    if (queueHandle != NULL) {
        errorCodeOrFree = (int32_t) uxQueueSpacesAvailable((QueueHandle_t) queueHandle);
    }

    return errorCodeOrFree;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: MUTEXES
 * -------------------------------------------------------------- */

// Create a mutex.
int32_t MTX_FN(uPortMutexCreate(uPortMutexHandle_t *pMutexHandle))
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (pMutexHandle != NULL) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        // Actually create the mutex
        *pMutexHandle = (uPortMutexHandle_t) xSemaphoreCreateMutex();
        if (*pMutexHandle != NULL) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Destroy a mutex.
int32_t MTX_FN(uPortMutexDelete(const uPortMutexHandle_t mutexHandle))
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (mutexHandle != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t) mutexHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

// Lock the given mutex.
int32_t MTX_FN(uPortMutexLock(const uPortMutexHandle_t mutexHandle))
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (mutexHandle != NULL) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xSemaphoreTake((SemaphoreHandle_t) mutexHandle,
                           (portTickType) portMAX_DELAY) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Try to lock the given mutex.
int32_t MTX_FN(uPortMutexTryLock(const uPortMutexHandle_t mutexHandle, int32_t delayMs))
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (mutexHandle != NULL) {
        errorCode = U_ERROR_COMMON_TIMEOUT;
        if (xSemaphoreTake((SemaphoreHandle_t) mutexHandle,
                           delayMs / portTICK_PERIOD_MS) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Unlock the given mutex.
int32_t MTX_FN(uPortMutexUnlock(const uPortMutexHandle_t mutexHandle))
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (mutexHandle != NULL) {
        xSemaphoreGive((SemaphoreHandle_t) mutexHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: SEMAPHORES
 * -------------------------------------------------------------- */

// Create a semaphore.
int32_t uPortSemaphoreCreate(uPortSemaphoreHandle_t *pSemaphoreHandle,
                             uint32_t initialCount,
                             uint32_t limit)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if ((pSemaphoreHandle != NULL) && (limit != 0) && (initialCount <= limit)) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        // Actually create the semaphore
        *pSemaphoreHandle = (uPortSemaphoreHandle_t) xSemaphoreCreateCounting(limit, initialCount);
        if (*pSemaphoreHandle != NULL) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Destroy a semaphore.
int32_t uPortSemaphoreDelete(const uPortSemaphoreHandle_t semaphoreHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (semaphoreHandle != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t) semaphoreHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

// Take the given semaphore.
int32_t uPortSemaphoreTake(const uPortSemaphoreHandle_t semaphoreHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (semaphoreHandle != NULL) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xSemaphoreTake((SemaphoreHandle_t) semaphoreHandle,
                           (portTickType) portMAX_DELAY) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Try to take the given semaphore.
int32_t uPortSemaphoreTryTake(const uPortSemaphoreHandle_t semaphoreHandle,
                              int32_t delayMs)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (semaphoreHandle != NULL) {
        errorCode = U_ERROR_COMMON_TIMEOUT;
        if (xSemaphoreTake((SemaphoreHandle_t) semaphoreHandle,
                           (delayMs / portTICK_PERIOD_MS)) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    return (int32_t) errorCode;
}

// Give the semaphore.
int32_t uPortSemaphoreGive(const uPortSemaphoreHandle_t semaphoreHandle)
{
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (semaphoreHandle != NULL) {
        xSemaphoreGive((SemaphoreHandle_t) semaphoreHandle);
        errorCode = U_ERROR_COMMON_SUCCESS;
    }

    return (int32_t) errorCode;
}

// Give the semaphore from interrupt.
int32_t uPortSemaphoreGiveIrq(const uPortSemaphoreHandle_t semaphoreHandle)
{
    BaseType_t yield = false;
    uErrorCode_t errorCode = U_ERROR_COMMON_INVALID_PARAMETER;

    if (semaphoreHandle != NULL) {
        errorCode = U_ERROR_COMMON_PLATFORM;
        if (xSemaphoreGiveFromISR((uPortSemaphoreHandle_t) semaphoreHandle,
                                  &yield) == pdTRUE) {
            errorCode = U_ERROR_COMMON_SUCCESS;
        }
    }

    // Required for correct FreeRTOS operation
    if (yield) {
        taskYIELD();
    }

    return (int32_t) errorCode;
}

// End of file
