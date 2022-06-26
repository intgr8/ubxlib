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

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of the configuration API for GNSS.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memcpy()

#include "u_error_common.h"

#include "u_port.h"
#include "u_port_os.h"  // Required by u_gnss_private.h

#include "u_ubx_protocol.h"

#include "u_gnss_module_type.h"
#include "u_gnss_type.h"
#include "u_gnss_private.h"
#include "u_gnss_cfg.h"

#include "intgr8_ubxlib_config.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Get the contents of UBX-CFG-NAV5.
// pBuffer must point to a buffer of length 36 bytes.
static int32_t uGnssCfgGetUbxCfgNav5(int32_t gnssHandle,
                                     char *pBuffer)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            errorCode = (int32_t) U_ERROR_COMMON_PLATFORM;
            // Poll with the message class and ID of the
            // UBX-CFG-NAV5 message
            if (uGnssPrivateSendReceiveUbxMessage(pInstance,
                                                  0x06, 0x24,
                                                  NULL, 0,
                                                  pBuffer, 36) == 36) {
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
            }
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCode;
}

// Set the contents of UBX-CFG-NAV5.
static int32_t uGnssCfgSetUbxCfgNav5(int32_t gnssHandle,
                                     uint16_t mask,
                                     const char *pBuffer,
                                     size_t size,
                                     size_t offset)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;
    // Enough room for the body of the UBX-CFG-NAV5 message
    char message[36] = {0};

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            // Set the mask bytes at the start of the message
            *((uint16_t *) message) = uUbxProtocolUint16Encode(mask);
            // Copy in the contents, which must have already
            // been correctly encoded
            memcpy(message + offset, pBuffer, size);
            // Send the UBX-CFG-NAV5 message
            errorCode = uGnssPrivateSendUbxMessage(pInstance,
                                                   0x06, 0x24,
                                                   message,
                                                   sizeof(message));
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCode;
}

#if ENABLE_CFG_SET_ANT_OFF
int32_t uGnssCfgSetANTOff(int32_t gnssHandle)
{
  // Sequence B5 62 06 41 0C 00 00 00 03 1F 90 47 4F B1 FF FF EA FF 33 98
  char message[36];
  // message[0] = 0x0C;
  // message[1] = 0x00;
  message[0] = 0x00;
  message[1] = 0x00;
  message[2] = 0x03;
  message[3] = 0x1F;
  message[4] = 0x90;
  message[5] = 0x47;
  message[6] = 0x4F;
  message[7] = 0xB1;
  message[8] = 0xFF;
  message[9] = 0xFF;
  message[10] = 0xEA;
  message[11] = 0xFF;
  int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
  uGnssPrivateInstance_t* pInstance;

  if (gUGnssPrivateMutex != NULL) {

      U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

      pInstance = pUGnssPrivateGetInstance(gnssHandle);
      if (pInstance != NULL) {
          errorCode = (int32_t) U_ERROR_COMMON_PLATFORM;
          // Poll with the message class and ID of the
          // UBX-CFG-NAV5 message
          // if (uGnssPrivateSendReceiveUbxMessage(pInstance,
          //                                       0x06, 0x24,
          //                                       NULL, 0,
          //                                       message, 36) == 36) {
          if (uGnssPrivateSendRawMessage(pInstance, 0x06, 0x41, message, 12u))
          {
              errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
          }
      }

      U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
  }

  return errorCode;

}
#endif // ENABLE_CFG_SET_ANT_OFF


/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Get the dynamic platform model from the GNSS chip.
int32_t uGnssCfgGetDynamic(int32_t gnssHandle)
{
    int32_t errorCodeOrDynamic;
    // Enough room for the body of the UBX-CFG-NAV5 message
    char message[36];

    errorCodeOrDynamic = uGnssCfgGetUbxCfgNav5(gnssHandle, message);
    if (errorCodeOrDynamic == 0) {
        // The dynamic platform model is at offset 2
        errorCodeOrDynamic = message[2];
    }

    return errorCodeOrDynamic;
}

// Set the dynamic platform model of the GNSS chip.
int32_t uGnssCfgSetDynamic(int32_t gnssHandle, uGnssDynamic_t dynamic)
{
    return uGnssCfgSetUbxCfgNav5(gnssHandle,
                                 0x01, /* Mask for dynamic model */
                                 (char *) &dynamic,
                                 1, 2 /* One byte at offset 2 */);
}

// Get the fix mode from the GNSS chip.
int32_t uGnssCfgGetFixMode(int32_t gnssHandle)
{
    int32_t errorCodeOrFixMode;
    // Enough room for the body of the UBX-CFG-NAV5 message
    char message[36];

    errorCodeOrFixMode = uGnssCfgGetUbxCfgNav5(gnssHandle, message);
    if (errorCodeOrFixMode == 0) {
        // The fix mode is at offset 3
        errorCodeOrFixMode = message[3];
    }

    return errorCodeOrFixMode;
}

// Set the fix mode of the GNSS chip.
int32_t uGnssCfgSetFixMode(int32_t gnssHandle, uGnssFixMode_t fixMode)
{
    return uGnssCfgSetUbxCfgNav5(gnssHandle,
                                 0x04, /* Mask for fix mode */
                                 (char *) &fixMode,
                                 1, 3 /* One byte at offset 3 */);
}

// End of file
