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

#ifndef _U_CFG_OS_PLATFORM_SPECIFIC_H_
#define _U_CFG_OS_PLATFORM_SPECIFIC_H_

/* No #includes allowed here */

/** @file
 * @brief  This header file contains OS configuration information for
 * an ESP32 platform.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS FOR ESP32: HEAP
 * -------------------------------------------------------------- */

/** Not stricty speaking part of the OS but there's nowhere better
 * to put this.  Set this to 1 if the C library does not free memory
 * that it has alloced internally when a task is deleted.
 * For instance, newlib when it is compiled in a certain way
 * does this on some platforms.
 */
#define U_CFG_OS_CLIB_LEAKS 0

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS FOR ESP32: OS GENERIC
 * -------------------------------------------------------------- */

#ifndef U_CFG_OS_PRIORITY_MIN
/** The minimum task priority.
 * In FreeRTOS, as used on this platform, low numbers indicate
 * lower priority.
 */
# define U_CFG_OS_PRIORITY_MIN 0
#endif

#ifndef U_CFG_OS_PRIORITY_MAX
/** The maximum task priority, should be less than or
 * equal to configMAX_PRIORITIES defined in FreeRTOSConfig.h,
 * which is usually set to 25.
 */
# define U_CFG_OS_PRIORITY_MAX 25
#endif

#ifndef U_CFG_OS_YIELD_MS
# ifndef ARDUINO
/** The amount of time to block for to ensure that a yield
 * occurs. This set to 20 ms as the ESP-IDF platform has a
 * 10 ms tick.
 */
#  define U_CFG_OS_YIELD_MS 20
# else
/** The amount of time to block for to ensure that a yield
 * occurs, just 2 ms on Arduino as they set a 1 ms tick.
 */
#  define U_CFG_OS_YIELD_MS 2
# endif
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS FOR ESP32: PRIORITIES
 * -------------------------------------------------------------- */

#ifndef ARDUINO
/** How much stack the task running all the examples and tests needs
 * in bytes, taking the value from our sdkconfig.defaults file.
 */
# define U_CFG_OS_APP_TASK_STACK_SIZE_BYTES CONFIG_MAIN_TASK_STACK_SIZE
#else
/** When built under Arduino it is not possible to provide a
 * sketch-specific sdkconfig with the correct CONFIG_MAIN_TASK_STACK_SIZE
 * for the application which runs the examples and tests so, since we
 * have to start if off in a task of its own anyway, we specify the
 * task stack size explicitly here and end the main task after
 * kicking the new one off.
 */
# define U_CFG_OS_APP_TASK_STACK_SIZE_BYTES 8192
#endif

/** The priority of the task running the examples and tests: should
 * be low but must be higher than the minimum.
 */
#define U_CFG_OS_APP_TASK_PRIORITY CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT

#endif // _U_CFG_OS_PLATFORM_SPECIFIC_H_

// End of file
