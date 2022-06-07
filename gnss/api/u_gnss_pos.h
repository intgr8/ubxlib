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

#ifndef _U_GNSS_POS_H_
#define _U_GNSS_POS_H_

/* No #includes allowed here */

/** @file
 * @brief This header file defines the GNSS APIs to read position.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#ifndef U_GNSS_POS_TIMEOUT_SECONDS
/** The timeout for position establishment in seconds.
 */
// # define U_GNSS_POS_TIMEOUT_SECONDS 240
# define U_GNSS_POS_TIMEOUT_SECONDS   1080
#endif

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Get the current position, returning on success or when
 * pKeepGoingCallback returns false.
 *
 * @param gnssHandle                  the handle of the GNSS instance
 *                                    to use.
 * @param pLatitudeX1e7               a place to put latitude (in ten
 *                                    millionths of a degree); may
 *                                    be NULL.
 * @param pLongitudeX1e7              a place to put longitude (in ten
 *                                    millionths of a degree); may be
 *                                    NULL.
 * @param pAltitudeMillimetres        a place to put the altitude (in
 *                                    millimetres); may be NULL.
 * @param pRadiusMillimetres          a place to put the radius of
 *                                    position (in millimetres); may
 *                                    be NULL.  If the radius is
 *                                    unknown -1 will be returned.
 * @param pSpeedMillimetresPerSecond  a place to put the speed (in
 *                                    millimetres per second); may be
 *                                    NULL.  If the speed is unknown
 *                                    -1 will be returned.
 * @param pSvs                        a place to store the number of
 *                                    space vehicles used in the
 *                                    solution; may be NULL. If the
 *                                    number of space vehicles is
 *                                    unknown or irrelevant -1 will
 *                                    be returned.
 * @param pTimeUtc                    a place to put the UTC time;
 *                                    may be NULL. If the time is
 *                                    unknown -1 will be returned.
 *                                    Note that this is the time of
 *                                    the fix and, by the time the
 *                                    fix is returned, it may not
 *                                    represent the *current* time.
 *                                    Note that this value may be
 *                                    populated even if the return
 *                                    value of the function is not
 *                                    success, since time may be
 *                                    available even if a position
 *                                    fix is not.
 * @param pKeepGoingCallback          a callback function that governs
 *                                    how long position-fixing is
 *                                    allowed to take. This function
 *                                    is called while waiting for
 *                                    position establishment to complete;
 *                                    position establishment will only
 *                                    continue while it returns true.
 *                                    This allows the caller to terminate
 *                                    the locating process at their
 *                                    convenience. This function may
 *                                    also be used to feed any watchdog
 *                                    timer that might be running. May
 *                                    be NULL, in which case position
 *                                    establishment will stop when
 *                                    U_GNSS_POS_TIMEOUT_SECONDS have
 *                                    elapsed.  The single int32_t
 *                                    parameter is the GNSS handle.
 * @return                            zero on success or negative error
 *                                    code on failure.
 */
int32_t uGnssPosGet(int32_t gnssHandle,
                    int32_t *pLatitudeX1e7, int32_t *pLongitudeX1e7,
                    int32_t *pAltitudeMillimetres,
                    int32_t *pRadiusMillimetres,
                    int32_t *pSpeedMillimetresPerSecond,
                    int32_t *pSvs, int64_t *pTimeUtc,
                    bool (*pKeepGoingCallback) (int32_t));

/** Get the current position, non-blocking version.  Note that
 * this function creates a mutex for thread-safety which remains
 * in memory even after pCallback has been called; calling
 * uGnssPosGetStop() will free it again.
 *
 * @param gnssHandle the handle of the GNSS instance to use.
 * @param pCallback  a callback that will be called when a fix has been
 *                   obtained.  The parameters to the callback are as
 *                   described in uGnssPosGet() except that they are
 *                   not pointers.  The position fix is only valid
 *                   if the second int32_t, errorCode, is zero but
 *                   a timeUtc value may still be included even
 *                   if a position fix has failed (timeUtc will be
 *                   set to -1 if the UTC time is not valid).
 *                   Note: don't call back into this API from your
 *                   pCallback, it could lead to recursion.
 * @return           zero on success or negative error code on
 *                   failure.
 */
int32_t uGnssPosGetStart(int32_t gnssHandle,
                         void (*pCallback) (int32_t gnssHandle,
                                            int32_t errorCode,
                                            int32_t latitudeX1e7,
                                            int32_t longitudeX1e7,
                                            int32_t altitudeMillimetres,
                                            int32_t radiusMillimetres,
                                            int32_t speedMillimetresPerSecond,
                                            int32_t svs,
                                            int64_t timeUtc));

/** Cancel a uGnssPosGetStart(); after this function has returned the
 * callback passed to uGnssPosGetStart() will not be called until another
 * uGnssPosGetStart() is begun.  uGnssPosGetStart() also creates a mutex
 * for thread safety which will remain in the system even after
 * pCallback has been called; this will free the memory it occupies.
 *
 * @param gnssHandle  the handle of the GNSS instance.
 */
void uGnssPosGetStop(int32_t gnssHandle);

#ifdef __cplusplus
}
#endif

#endif // _U_GNSS_POS_H_

// End of file
