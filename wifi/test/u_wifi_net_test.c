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
 * @brief Tests for the wifi NET API: TODO
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stdint.h"    // int32_t etc.

// Must always be included before u_short_range_test_selector.h
//lint -efile(766, u_wifi_module_type.h)
#include "u_wifi_module_type.h"

#include "u_short_range_test_selector.h"

#if U_SHORT_RANGE_TEST_WIFI()

#include "stddef.h"    // NULL, size_t etc.
#include "stdbool.h"

#include "u_cfg_sw.h"
#include "u_cfg_app_platform_specific.h"
#include "u_cfg_test_platform_specific.h"
#include "u_cfg_os_platform_specific.h"  // For #define U_CFG_OS_CLIB_LEAKS

#include "u_port.h"
#include "u_port_debug.h"
#include "u_port_os.h"
//lint -efile(766, u_port_uart.h) Suppress header file not used
#include "u_port_uart.h"

#include "u_error_common.h"

#include "u_at_client.h"

#include "u_wifi.h"
#include "u_wifi_net.h"
#include "u_wifi_test_private.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

//lint -esym(767, LOG_TAG) Suppress LOG_TAG defined differently in another module
//lint -esym(750, LOG_TAG) Suppress LOG_TAG not referenced
#define LOG_TAG "U_SHORT_RANGE_TEST: "

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

static uWifiTestPrivate_t gHandles = { -1, -1, NULL, -1 };

static const uint32_t gNetStatusMaskAllUp = U_WIFI_NET_STATUS_MASK_IPV4_UP |
                                            U_WIFI_NET_STATUS_MASK_IPV6_UP;

static volatile int32_t gWifiConnected = 0;
static volatile int32_t gWifiDisconnected = 0;
static volatile uint32_t gNetStatusMask = 0;
static volatile int32_t gLookForDisconnectReasonBitMask = 0;
static volatile int32_t gDisconnectReasonFound = 0;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

static void wifiConnectionCallback(int32_t wifiHandle,
                                   int32_t connId,
                                   int32_t status,
                                   int32_t channel,
                                   char *pBssid,
                                   int32_t disconnectReason,
                                   void *pCallbackParameter)
{
    (void)wifiHandle;
    (void)pBssid;
    (void)pCallbackParameter;
    (void)channel;
    (void)connId;
    if (status == U_WIFI_NET_CON_STATUS_CONNECTED) {
#if !U_CFG_OS_CLIB_LEAKS
        uPortLog(LOG_TAG "Connected Wifi connId: %d, bssid: %s, channel: %d\n",
                 connId,
                 pBssid,
                 channel);
#endif
        gWifiConnected = 1;
    } else {
#if defined(U_CFG_ENABLE_LOGGING) && !U_CFG_OS_CLIB_LEAKS
        //lint -esym(752, strDisconnectReason)
        static const char strDisconnectReason[6][20] = {
            "Unknown", "Remote Close", "Out of range",
            "Roaming", "Security problems", "Network disabled"
        };
        if ((disconnectReason < 0) && (disconnectReason >= 6)) {
            // For all other values use "Unknown"
            disconnectReason = 0;
        }
        uPortLog(LOG_TAG "Wifi connection lost connId: %d, reason: %d (%s)\n",
                 connId,
                 disconnectReason,
                 strDisconnectReason[disconnectReason]);
#endif
        gWifiConnected = 0;
        gWifiDisconnected = 1;
        if (((1ul << disconnectReason) & gLookForDisconnectReasonBitMask) > 0) {
            gDisconnectReasonFound = 1;
        }
    }
}

static void wifiNetworkStatusCallback(int32_t wifiHandle,
                                      int32_t interfaceType,
                                      uint32_t statusMask,
                                      void *pCallbackParameter)
{
    (void)wifiHandle;
    (void)interfaceType;
    (void)statusMask;
    (void)pCallbackParameter;
#if !U_CFG_OS_CLIB_LEAKS
    uPortLog(LOG_TAG "Network status IPv4 %s, IPv6 %s\n",
             ((statusMask & U_WIFI_NET_STATUS_MASK_IPV4_UP) > 0) ? "up" : "down",
             ((statusMask & U_WIFI_NET_STATUS_MASK_IPV6_UP) > 0) ? "up" : "down");
#endif

    gNetStatusMask = statusMask;
}


static uWifiTestError_t runWifiTest(const char *pSsid, const char *pPassPhrase)
{
    uWifiTestError_t testError = U_WIFI_TEST_ERROR_NONE;
    uWifiTestError_t connectError = U_WIFI_TEST_ERROR_NONE;
    uWifiTestError_t disconnectError = U_WIFI_TEST_ERROR_NONE;
    int32_t waitCtr = 0;
    gNetStatusMask = 0;
    gWifiConnected = 0;
    gWifiDisconnected = 0;
    // Do the standard preamble
    if (0 != uWifiTestPrivatePreamble((uWifiModuleType_t) U_CFG_TEST_SHORT_RANGE_MODULE_TYPE,
                                      &gHandles)) {
        testError = U_WIFI_TEST_ERROR_PREAMBLE;
    }

    if (testError == U_WIFI_TEST_ERROR_NONE) {
        // Add unsolicited response cb for connection status
        uWifiNetSetConnectionStatusCallback(gHandles.wifiHandle,
                                            wifiConnectionCallback, NULL);
        // Add unsolicited response cb for IP status
        uWifiNetSetNetworkStatusCallback(gHandles.wifiHandle,
                                         wifiNetworkStatusCallback, NULL);
        // Connect to wifi network
        int32_t res = uWifiNetStationConnect(gHandles.wifiHandle,
                                             pSsid,
                                             U_WIFI_NET_AUTH_WPA_PSK,
                                             pPassPhrase);
        if (res == 0) {
            //Wait for connection and IP events.
            //There could be multiple IP events depending on network configuration.
            while (!connectError && (!gWifiConnected || (gNetStatusMask != gNetStatusMaskAllUp))) {
                if (waitCtr >= 15) {
                    if (!gWifiConnected) {
                        uPortLog(LOG_TAG "Unable to connect to WifiNetwork\n");
                        connectError = U_WIFI_TEST_ERROR_CONNECTED;
                    } else {
                        uPortLog(LOG_TAG "Unable to retrieve IP address\n");
                        connectError = U_WIFI_TEST_ERROR_IPRECV;
                    }
                    break;
                }

                uPortTaskBlock(1000);
                waitCtr++;
            }
        } else {
            connectError = U_WIFI_TEST_ERROR_CONNECT;
        }
    }

    if (testError == U_WIFI_TEST_ERROR_NONE) {
        // Disconnect from wifi network (regardless of previous connectError)
        if (uWifiNetStationDisconnect(gHandles.wifiHandle) == 0) {
            waitCtr = 0;
            while (!disconnectError && (!gWifiDisconnected || (gNetStatusMask > 0))) {
                if (waitCtr >= 5) {
                    disconnectError = U_WIFI_TEST_ERROR_DISCONNECT;
                    if (!gWifiDisconnected) {
                        uPortLog(LOG_TAG "Unable to diconnect from WifiNetwork");
                    } else {
                        uPortLog(LOG_TAG "Network status is still up");
                    }
                    break;
                }
                uPortTaskBlock(1000);
                waitCtr++;
            }
        } else {
            disconnectError = U_WIFI_TEST_ERROR_DISCONNECT;
        }
    }

    // Aggregate result
    if (testError == U_WIFI_TEST_ERROR_NONE) {
        if (connectError != U_WIFI_TEST_ERROR_NONE) {
            testError = connectError;
        } else {
            testError = disconnectError;
        }
    }

    // Cleanup
    uWifiNetSetConnectionStatusCallback(gHandles.wifiHandle,
                                        NULL, NULL);
    uWifiNetSetNetworkStatusCallback(gHandles.wifiHandle,
                                     NULL, NULL);
    uWifiTestPrivatePostamble(&gHandles);
    return testError;
}

U_PORT_TEST_FUNCTION("[wifiNet]", "wifiNetInitialisation")
{
    int32_t waitCtr = 0;
    gNetStatusMask = 0;
    gWifiConnected = 0;
    gWifiDisconnected = 0;
    uWifiTestError_t testError = U_WIFI_TEST_ERROR_NONE;

    // Previous test may have left wifi connected
    // For this reason we start with making sure the wifi gets disconnected here

    // Do the standard preamble
    if (0 != uWifiTestPrivatePreamble((uWifiModuleType_t) U_CFG_TEST_SHORT_RANGE_MODULE_TYPE,
                                      &gHandles)) {
        testError = U_WIFI_TEST_ERROR_PREAMBLE;
    }

    if (!testError) {
        // Add unsolicited response cb for connection status
        uWifiNetSetConnectionStatusCallback(gHandles.wifiHandle,
                                            wifiConnectionCallback, NULL);
        // Add unsolicited response cb for IP status
        uWifiNetSetNetworkStatusCallback(gHandles.wifiHandle,
                                         wifiNetworkStatusCallback, NULL);
    }

    if (!testError) {
        if (uWifiNetStationDisconnect(gHandles.wifiHandle) == 0) {
            waitCtr = 0;
            while (!testError && (!gWifiDisconnected || (gNetStatusMask > 0))) {
                if (waitCtr >= 5) {
                    break;
                }
                uPortTaskBlock(1000);
                waitCtr++;
            }
        } else {
            testError = U_WIFI_TEST_ERROR_DISCONNECT;
        }
    }

    // Cleanup
    uWifiNetSetConnectionStatusCallback(gHandles.wifiHandle,
                                        NULL, NULL);
    uWifiNetSetNetworkStatusCallback(gHandles.wifiHandle,
                                     NULL, NULL);
    uWifiTestPrivatePostamble(&gHandles);

    U_PORT_TEST_ASSERT(testError == U_WIFI_TEST_ERROR_NONE);
}

U_PORT_TEST_FUNCTION("[wifiNet]", "wifiNetStationConnect")
{
    uWifiTestError_t testError = runWifiTest(U_PORT_STRINGIFY_QUOTED(U_WIFI_TEST_CFG_SSID),
                                             U_PORT_STRINGIFY_QUOTED(U_WIFI_TEST_CFG_WPA2_PASSPHRASE));
    // Handle errors
    U_PORT_TEST_ASSERT(testError == U_WIFI_TEST_ERROR_NONE);
}

U_PORT_TEST_FUNCTION("[wifiNet]", "wifiNetStationConnectWrongSSID")
{
    gLookForDisconnectReasonBitMask = (1 << U_WIFI_NET_REASON_OUT_OF_RANGE); // (cant find SSID)
    gDisconnectReasonFound = 0;
    uWifiTestError_t testError = runWifiTest("DUMMYSSID",
                                             U_PORT_STRINGIFY_QUOTED(U_WIFI_TEST_CFG_WPA2_PASSPHRASE));

    // Handle errors
    U_PORT_TEST_ASSERT(testError == U_WIFI_TEST_ERROR_CONNECTED);
    U_PORT_TEST_ASSERT(gDisconnectReasonFound);
}

U_PORT_TEST_FUNCTION("[wifiNet]", "wifiNetStationConnectWrongPassphrase")
{
    // The expected disconnect reason is U_WIFI_NET_REASON_SECURITY_PROBLEM.
    // However, for some APs we will only get U_WIFI_NET_REASON_UNKNOWN.
    gLookForDisconnectReasonBitMask = (1 << U_WIFI_NET_REASON_UNKNOWN) |
                                      (1 << U_WIFI_NET_REASON_SECURITY_PROBLEM);
    gDisconnectReasonFound = 0;
    uWifiTestError_t testError = runWifiTest(U_PORT_STRINGIFY_QUOTED(U_WIFI_TEST_CFG_SSID),
                                             "WRONGPASSWD");
    // Handle errors
    U_PORT_TEST_ASSERT(testError == U_WIFI_TEST_ERROR_CONNECTED);
    U_PORT_TEST_ASSERT(gDisconnectReasonFound);
}

#endif // U_SHORT_RANGE_TEST_WIFI()
// End of file
