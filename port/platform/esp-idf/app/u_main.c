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
 * @brief The application entry point for the ESP32 platform.  Starts
 * the platform and calls Unity to run the selected examples/tests.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"

#include "u_cfg_sw.h"
#include "u_cfg_app_platform_specific.h"
#include "u_cfg_test_platform_specific.h"

#include "u_port.h"
#include "u_port_os.h"
#include "u_port_debug.h"

#ifdef U_RUNNER_TOP_STR
# include "u_runner.h"
#endif

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

// Do things via Unity.
static void appTask(void *pParam)
{
    (void) pParam;

#ifdef U_CFG_MUTEX_DEBUG
    uMutexDebugInit();
    uMutexDebugWatchdog(uMutexDebugPrint, NULL,
                        U_MUTEX_DEBUG_WATCHDOG_TIMEOUT_SECONDS);
#endif

#ifdef U_RUNNER_TOP_STR
    // If U_RUNNER_TOP_STR is defined we must be running inside the
    // test automation system (since the definition is added by
    // u_run.py) so run the tests through u_runner as that allows us
    // to do filtering

    uPortInit();

    UNITY_BEGIN();

    uPortLog("U_APP: functions available:\n\n");
    uRunnerPrintAll("U_APP: ");
# ifdef U_CFG_APP_FILTER
    uPortLog("U_APP: running functions that begin with \"%s\".\n",
             U_PORT_STRINGIFY_QUOTED(U_CFG_APP_FILTER));
    uRunnerRunFiltered(U_PORT_STRINGIFY_QUOTED(U_CFG_APP_FILTER),
                       "U_APP: ");
# else
    uPortLog("U_APP: running all functions.\n");
    uRunnerRunAll("U_APP: ");
# endif

    // The things that we have run may have
    // called deinit so call init again here.
    uPortInit();

    UNITY_END();

    uPortDeinit();

    while (1) {}
#else
    // If U_RUNNER_TOP_STR is not defined we must be running outside
    // the test automation environment so call the normal ESP32
    // menu system
    unity_run_menu();
#endif
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Application entry point.
void app_main(void)
{
    // On this platform the OS is started automagically so we
    // don't need to worry about stack sizes or priority
    uPortPlatformStart(appTask, NULL, 0, 0);

    // Should never get here
    assert(false);
}

// End of file
