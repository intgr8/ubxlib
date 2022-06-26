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

#ifndef _U_GNSS_CFG_H_
#define _U_GNSS_CFG_H_

/* No #includes allowed here */

/** @file
 * @brief This header file defines the GNSS APIs to configure a GNSS chip.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Get the dynamic platform model from the GNSS chip.
 *
 * @param gnssHandle  the handle of the GNSS instance.
 * @return            the number of the dynamic platform model or
 *                    negative error code.
 */
int32_t uGnssCfgGetDynamic(int32_t gnssHandle);

/** Set the dynamic platform model of the GNSS chip.
 *
 * @param gnssHandle  the handle of the GNSS instance.
 * @param dynamic     the number of the dynamic platform model; the
 *                    value is deliberately not range-checked to allow
 *                    future dynamic platform models to be passed
 *                    in without the requirement to modify this code.
 * @return            zero on succes or negative error code.
 */
int32_t uGnssCfgSetDynamic(int32_t gnssHandle, uGnssDynamic_t dynamic);

/** Get the fix mode from the GNSS chip.
 *
 * @param gnssHandle  the handle of the GNSS instance.
 * @return            the fix mode or negative error code.
 */
int32_t uGnssCfgGetFixMode(int32_t gnssHandle);

/** Set the fix mode of the GNSS chip.
 *
 * @param gnssHandle  the handle of the GNSS instance.
 * @param fixMode     the fix mode; the value is deliberately not
 *                    range-checked to allow future fix modes to be
 *                    passed in without the requirement to modify
 *                    this code.
 * @return            zero on succes or negative error code.
 */
int32_t uGnssCfgSetFixMode(int32_t gnssHandle, uGnssFixMode_t fixMode);

/**
 * Invert Active-low logic of ANT-OFF to active-high
 *
 * @warning This is a one way door. Once called, it should not be called anymore
 * and it is irreversible.
 *
 * @note By default the code is disabled. Set @c ENABLE_CFG_SET_ANT_OFF in the
 * ublox-intgr8 config file, @file  intgr8_ubxlib_config.h
 * @param gnssHandle  the handle of the GNSS instance.
 * @return zero on succes or negative error code.
 */
int32_t uGnssCfgSetANTOff(int32_t gnssHandle);

#ifdef __cplusplus
}
#endif

#endif // _U_GNSS_CFG_H_

// End of file
