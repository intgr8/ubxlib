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
 * @brief Test for the port API: these should pass on all platforms.
 * IMPORTANT: see notes in u_cfg_test_platform_specific.h for the
 * naming rules that must be followed when using the U_PORT_TEST_FUNCTION()
 * macro.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "stdlib.h"    // rand()
#include "string.h"    // strlen(), memcmp()
#include "stdio.h"     // snprintf()
#include "ctype.h"     // isprint()

#include "u_cfg_sw.h"
#include "u_cfg_os_platform_specific.h"  // For #define U_CFG_OS_CLIB_LEAKS
#include "u_cfg_app_platform_specific.h"
#include "u_cfg_test_platform_specific.h"

#include "u_error_common.h"

#include "u_port_clib_platform_specific.h" /* Integer stdio, must be included
                                              before the other port files if
                                              any print or scan function is used. */
#include "u_port.h"
#include "u_port_debug.h"
#include "u_port_os.h"
#include "u_port_uart.h"

#include "u_at_client.h"
#include "u_at_client_test.h"
#include "u_at_client_test_data.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/** The CME/CMS ERROR number to use during testing.
 */
#define U_AT_CLIENT_TEST_CMX_ERROR_NUMBER 65535

/** The size required of a malloc()ed buffer for the
 * AT server.  This must be big enough for all of the lines
 * of response in any one uAtClientTestCommandResponse_t of
 * gAtClientTestSet[], including multiple copies of the URC
 * (as many as there are lines in the response plus a few);
 * so quite big
 */
#define U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH 2048

/** The size of buffer required for response/URC checking.
 * Big enough for each individual string/byte parameter in
 * the test data.
 */
#define U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH 512

/** An AT timeout to use during testing; make sure that this
 * is longer than that used in gAtClientTestEchoTimeout.
 */
#define U_AT_CLIENT_TEST_AT_TIMEOUT_MS 2000

/** The tolerance allowed on the AT timeout in milliseconds.
 */
#define U_AT_CLIENT_TEST_AT_TIMEOUT_TOLERANCE_MS 250

/** The AT client buffer length to use during testing:
 * we send non-prefixed response of length 256 bytes plus
 * we need room for initial and trailing line endings. */
#define U_AT_CLIENT_TEST_AT_BUFFER_LENGTH_BYTES (256 + 4 + U_AT_CLIENT_BUFFER_OVERHEAD_BYTES)

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Data structure to keep track of checking the
 * commands and response.
 */
typedef struct {
    const uAtClientTestCommandResponse_t *pTestSet;
    size_t index;
    size_t commandPassIndex;
    int32_t commandLastError;
    size_t responsePassIndex;
    int32_t responseLastError;
} uAtClientTestCheckCommandResponse_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/** Handle for the AT client UART stream.
 */
static int32_t gUartAHandle = -1;

/** Handle for the AT server UART stream (i.e. the reverse direction).
 */
static int32_t gUartBHandle = -1;

#if (U_CFG_TEST_UART_A >= 0)

/** Store the last consecutive AT time-out call-back here.
 */
static int32_t gConsecutiveTimeout;

/** For tracking heap lost to memory  lost by the C library.
 */
static size_t gSystemHeapLost = 0;

# if (U_CFG_TEST_UART_B >= 0)

/** AT server buffer used by atServerCallback() and atEchoServerCallback().
 */
static char gAtServerBuffer[1024];

/** Used by pInterceptTx.
 */
static const char *gpInterceptTxDataLast = NULL;

# endif
#endif

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

#if (U_CFG_TEST_UART_A >= 0)

// AT consecutive timeout callback, used by some of the tests below
//lint -e{818} suppress "could be declared as pointing to const", callback
// has to follow function signature
static void consecutiveTimeoutCallback(uAtClientHandle_t atHandle,
                                       int32_t *pCount)
{
    (void) atHandle;
#if U_CFG_OS_CLIB_LEAKS
    int32_t heapUsed = uPortGetHeapFree();
#endif

    uPortLog("U_AT_CLIENT_TEST: AT consecutive timeout callback"
             " called with %d.\n", *pCount);

#if U_CFG_OS_CLIB_LEAKS
    // Take account of any heap lost through the printf()
    gSystemHeapLost += (size_t) (unsigned) (heapUsed - uPortGetHeapFree());
#endif

    gConsecutiveTimeout = *pCount;
}

// Check the stack extents for the URC and callbacks tasks.
static void checkStackExtents(uAtClientHandle_t atHandle)
{
    int32_t stackMinFreeBytes;

    stackMinFreeBytes = uAtClientUrcHandlerStackMinFree(atHandle);
    if (stackMinFreeBytes != (int32_t) U_ERROR_COMMON_NOT_SUPPORTED) {
        uPortLog("U_AT_CLIENT_TEST: URC task had min %d byte(s)"
                 " stack free out of %d.\n", stackMinFreeBytes,
                 U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES);
        U_PORT_TEST_ASSERT(stackMinFreeBytes > 0);
    }

    stackMinFreeBytes = uAtClientCallbackStackMinFree();
    if (stackMinFreeBytes != (int32_t) U_ERROR_COMMON_NOT_SUPPORTED) {
        uPortLog("U_AT_CLIENT_TEST: AT callback task had min %d byte(s)"
                 " stack free out of %d.\n", stackMinFreeBytes,
                 U_AT_CLIENT_CALLBACK_TASK_STACK_SIZE_BYTES);
        U_PORT_TEST_ASSERT(stackMinFreeBytes > 0);
    }
}

# if (U_CFG_TEST_UART_B >= 0)

// The preamble for tests involving two UARTs.
static void twoUartsPreamble()
{
    gUartAHandle = uPortUartOpen(U_CFG_TEST_UART_A,
                                 U_CFG_TEST_BAUD_RATE,
                                 NULL,
                                 U_CFG_TEST_UART_BUFFER_LENGTH_BYTES,
                                 U_CFG_TEST_PIN_UART_A_TXD,
                                 U_CFG_TEST_PIN_UART_A_RXD,
                                 U_CFG_TEST_PIN_UART_A_CTS,
                                 U_CFG_TEST_PIN_UART_A_RTS);
    U_PORT_TEST_ASSERT(gUartAHandle >= 0);

    uPortLog("U_AT_CLIENT_TEST: AT client will be on UART %d,"
             " TXD pin %d (0x%02x) and RXD pin %d (0x%02x).\n",
             U_CFG_TEST_UART_A, U_CFG_TEST_PIN_UART_A_TXD,
             U_CFG_TEST_PIN_UART_A_TXD, U_CFG_TEST_PIN_UART_A_RXD,
             U_CFG_TEST_PIN_UART_A_RXD);

    gUartBHandle = uPortUartOpen(U_CFG_TEST_UART_B,
                                 U_CFG_TEST_BAUD_RATE,
                                 NULL,
                                 U_CFG_TEST_UART_BUFFER_LENGTH_BYTES,
                                 U_CFG_TEST_PIN_UART_B_TXD,
                                 U_CFG_TEST_PIN_UART_B_RXD,
                                 U_CFG_TEST_PIN_UART_B_CTS,
                                 U_CFG_TEST_PIN_UART_B_RTS);
    U_PORT_TEST_ASSERT(gUartBHandle >= 0);

    uPortLog("U_AT_CLIENT_TEST: AT server will be on UART %d,"
             " TXD pin %d (0x%02x) and RXD pin %d (0x%02x).\n",
             U_CFG_TEST_UART_B, U_CFG_TEST_PIN_UART_B_TXD,
             U_CFG_TEST_PIN_UART_B_TXD, U_CFG_TEST_PIN_UART_B_RXD,
             U_CFG_TEST_PIN_UART_B_RXD);

    uPortLog("U_AT_CLIENT_TEST: make sure these pins are cross-connected.\n");
}

// Check that an AT timeout is obeyed.
static bool atTimeoutIsObeyed(uAtClientHandle_t atClientHandle,
                              int32_t timeoutMs)
{
    bool success = false;
    int64_t startTime;
    int32_t duration;
    int32_t consecutiveTimeouts;
    int32_t x;
    int32_t y;

    startTime = uPortGetTickTimeMs();
    uAtClientLock(atClientHandle);
    // Send nothing
    consecutiveTimeouts = gConsecutiveTimeout;
    uAtClientCommandStart(atClientHandle, NULL);
    uAtClientCommandStop(atClientHandle);
    uAtClientResponseStart(atClientHandle, NULL);
    // Read should time out
    x = uAtClientReadInt(atClientHandle);
    uAtClientResponseStop(atClientHandle);
    y = uAtClientUnlock(atClientHandle);
    // Give consecutiveTimeoutCallback() chance
    // to complete
    uPortTaskBlock(U_CFG_OS_YIELD_MS);
    if ((x < 0) && (y < 0) &&
        (gConsecutiveTimeout == consecutiveTimeouts + 1)) {
        duration = (int32_t) (uPortGetTickTimeMs() - startTime);
        if ((duration < timeoutMs) ||
            (duration > timeoutMs + U_AT_CLIENT_TEST_AT_TIMEOUT_TOLERANCE_MS)) {
            uPortLog("U_AT_CLIENT_TEST: AT timeout was not obeyed"
                     " (%d ms as opposed to %d ms).\n",
                     (int) duration, timeoutMs);
        } else {
            success = true;
        }
    } else {
        uPortLog("U_AT_CLIENT_TEST: expected AT timeout error did not occur.\n");
    }

    return success;
}

// The URC handler for these tests.
static void urcHandler(uAtClientHandle_t atClientHandle, void *pParameters)
{
    uAtClientTestCheckUrc_t *pCheckUrc;
    const uAtClientTestResponseLine_t *pUrc;
    int32_t lastError = 0;

    // pParameters is the checking structure and in that is a pointer
    // to the definition of what should be in the URC
    pCheckUrc = (uAtClientTestCheckUrc_t *) pParameters;
    pUrc = pCheckUrc->pUrc;

    // Read all of the parameters and check them
    for (size_t p = 0; (p < pUrc->numParameters) && (lastError == 0); p++) {
        lastError = uAtClientTestCheckParam(atClientHandle,
                                            &(pUrc->parameters[p]), "_URC");
    }

    pCheckUrc->count++;
    if (pCheckUrc->lastError == 0) {
        pCheckUrc->lastError = lastError;
    }
    if (lastError == 0) {
        // This URC passes
        pCheckUrc->passIndex++;
    }
}

// Write to a buffer returning the number of bytes written
static size_t writeToBuffer(char *pBuffer, size_t bufferLength,
                            const char *pBytes, size_t length)
{
    if (length > bufferLength) {
        length = bufferLength;
    }

    memcpy(pBuffer, pBytes, length);

    return length;
}

// Assemble the start of a response that would come from an AT server
// into pBuffer.
static size_t createAtServerResponseStart(char *pBuffer,
                                          size_t bufferLength)
{
    return writeToBuffer(pBuffer, bufferLength,
                         U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                         strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
}

// Assemble one line of a response that would come from an AT server
// into pBuffer (which could also be a URC).
static size_t createAtServerResponseLine(char *pBuffer,
                                         size_t bufferLength,
                                         const uAtClientTestResponseLine_t *pLine)
{
    size_t writtenLength = 0;

    // Send the prefix for this line and a space to follow it
    if (pLine->pPrefix != NULL) {
        writtenLength = writeToBuffer(pBuffer, bufferLength,
                                      pLine->pPrefix,
                                      strlen(pLine->pPrefix));
        writtenLength += writeToBuffer(pBuffer + writtenLength,
                                       bufferLength - writtenLength,
                                       " ", 1);
    }
    // Send the parameters of the line, separated by delimiters
    for (size_t p = 0; p < pLine->numParameters; p++) {
        if (p > 0) {
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_DELIMITER,
                                           strlen(U_AT_CLIENT_TEST_DELIMITER));
        }
        writtenLength += writeToBuffer(pBuffer + writtenLength,
                                       bufferLength - writtenLength,
                                       pLine->parametersRaw[p].pBytes,
                                       pLine->parametersRaw[p].length);
    }
    // Terminate the line
    writtenLength += writeToBuffer(pBuffer + writtenLength,
                                   bufferLength - writtenLength,
                                   U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                   strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));

    return writtenLength;
}

// Assemble a line of URC
static size_t createAtServerResponseUrc(char *pBuffer,
                                        size_t bufferLength,
                                        const uAtClientTestResponseLine_t *pLine)
{
    size_t writtenLength = 0;

    if (pLine != NULL) {
        writtenLength = createAtServerResponseStart(pBuffer, bufferLength);
        writtenLength += createAtServerResponseLine(pBuffer + writtenLength,
                                                    bufferLength - writtenLength,
                                                    pLine);
    }

    return writtenLength;
}

// Assemble the end of a response that would come from an AT server
// into pBuffer.
static size_t createAtServerResponseEnd(char *pBuffer,
                                        size_t bufferLength,
                                        const uAtClientTestResponseType_t type,
                                        int32_t errorNum)
{
    size_t x;
    char buffer[10]; // Enough for the CME ERROR/CMS ERROR number
    size_t writtenLength = 0;

    // Assemble OK/(CMS)/(CME) ERROR
    switch (type) {
        case U_AT_CLIENT_TEST_RESPONSE_OK:
            writtenLength = writeToBuffer(pBuffer, bufferLength,
                                          U_AT_CLIENT_TEST_OK,
                                          strlen(U_AT_CLIENT_TEST_OK));
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                           strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
            break;
        case U_AT_CLIENT_TEST_RESPONSE_ERROR:
            writtenLength = writeToBuffer(pBuffer, bufferLength,
                                          U_AT_CLIENT_TEST_ERROR,
                                          strlen(U_AT_CLIENT_TEST_ERROR));
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                           strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
            break;
        case U_AT_CLIENT_TEST_RESPONSE_CME_ERROR:
            writtenLength = writeToBuffer(pBuffer, bufferLength,
                                          U_AT_CLIENT_TEST_CME_ERROR,
                                          strlen(U_AT_CLIENT_TEST_CME_ERROR));
            x = snprintf(buffer, sizeof(buffer), "%d", (int) errorNum);
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           buffer, x);
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                           strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
            break;
        case U_AT_CLIENT_TEST_RESPONSE_CMS_ERROR:
            writtenLength = writeToBuffer(pBuffer, bufferLength,
                                          U_AT_CLIENT_TEST_CMS_ERROR,
                                          strlen(U_AT_CLIENT_TEST_CMS_ERROR));
            x = snprintf(buffer, sizeof(buffer), "%d", (int) errorNum);
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           buffer, x);
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                           strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
            break;
        case U_AT_CLIENT_TEST_RESPONSE_ABORTED:
            writtenLength = writeToBuffer(pBuffer, bufferLength,
                                          U_AT_CLIENT_TEST_ABORTED,
                                          strlen(U_AT_CLIENT_TEST_ABORTED));
            writtenLength += writeToBuffer(pBuffer + writtenLength,
                                           bufferLength - writtenLength,
                                           U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                           strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
            break;
        case U_AT_CLIENT_TEST_RESPONSE_NONE:
        default:
            break;
    }

    return writtenLength;
}

// Callback to receive the output from the AT client through
// another UART cross-wired to it and return responses.
static void atServerCallback(int32_t uartHandle, uint32_t eventBitmask,
                             void *pParameters)
{
    int32_t sizeOrError;
    int32_t lastError = 0;
    uAtClientTestCheckCommandResponse_t *pCheckCommandResponse;
    char *pReceive = gAtServerBuffer;
    size_t receiveLength = 0;
    const uAtClientTestCommand_t *pCommand;
    const uAtClientTestResponse_t *pResponse;
    const uAtClientTestResponseLine_t *pUrc;
    const char *pBytes;
    size_t length;
    size_t increment;
    char *pBuffer;
    const char *pTmp;
#if U_CFG_OS_CLIB_LEAKS
    int32_t heapUsed;
#endif

    (void) uartHandle;

    if (eventBitmask & U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED) {
        pCheckCommandResponse = (uAtClientTestCheckCommandResponse_t *) pParameters;
        // Loop until no received characters left to process
        while ((uPortUartGetReceiveSize(uartHandle) > 0) && (lastError == 0)) {
            sizeOrError = uPortUartRead(uartHandle, pReceive,
                                        sizeof(gAtServerBuffer) -
                                        (pReceive - gAtServerBuffer));
            if (sizeOrError >= 0) {
                pReceive += sizeOrError;
                receiveLength += sizeOrError;
                if (receiveLength >= sizeof(gAtServerBuffer)) {
                    lastError = 1;
                }
            } else {
                lastError = sizeOrError;
            }
            // Wait long enough for everything to have been received
            // and for any prints in the sending task to be printed
            uPortTaskBlock(100);
        }

        if (receiveLength > 0) {
#if U_CFG_OS_CLIB_LEAKS
            // Calling printf() from a new task causes newlib
            // to allocate additional memory which, depending
            // on the OS/system, may not be recovered;
            // take account of that here.
            heapUsed = uPortGetHeapFree();
#endif

            uPortLog("U_AT_SERVER_TEST_%d: received command: \"",
                     pCheckCommandResponse->index + 1);
            uAtClientTestPrint(gAtServerBuffer, receiveLength);
            uPortLog("\".\n");

#if U_CFG_OS_CLIB_LEAKS
            // Take account of any heap lost through the first
            // printf()
            gSystemHeapLost += (size_t) (unsigned) (heapUsed - uPortGetHeapFree());
#endif

            // Check what we received
            pCommand = &(pCheckCommandResponse->pTestSet[pCheckCommandResponse->index].command);
            // First the command
            pBytes = pCommand->pString;
            length = strlen(pBytes);
            pReceive = gAtServerBuffer;
            if ((receiveLength >= length) &&
                (memcmp(pBytes, pReceive, length) == 0)) {
                pReceive += length;
                receiveLength -= length;
                // Then each parameter, separated by delimiters
                for (size_t x = 0; (x < pCommand->numParameters) && (lastError == 0); x++) {
                    // Note: if the command is a byte array with the standalone option
                    // then the delimiter check is skipped
                    if ((x > 0) &&
                        (pCommand->parameters[x].type !=
                         U_AT_CLIENT_TEST_PARAMETER_COMMAND_BYTES_STANDALONE)) {
                        // Check for delimiter
                        pBytes = U_AT_CLIENT_TEST_DELIMITER;
                        length = strlen(pBytes);
                        if ((receiveLength >= length) &&
                            (memcmp(pBytes, pReceive, length) == 0)) {
                            pReceive += length;
                            receiveLength -= length;
                        } else {
                            uPortLog("U_AT_SERVER_TEST_%d: expected delimiter (\"%s\")"
                                     " but received \"",
                                     pCheckCommandResponse->index + 1, pBytes);
                            uAtClientTestPrint(pReceive, length);
                            uPortLog("\".\n");
                            lastError = 3;
                        }
                    }
                    if (lastError == 0) {
                        pBytes = pCommand->parametersRaw[x].pBytes;
                        length = pCommand->parametersRaw[x].length;
                        if ((receiveLength >= length) &&
                            (memcmp(pBytes, pReceive, length) == 0)) {
                            pReceive += length;
                            receiveLength -= length;
                        } else {
                            uPortLog("U_AT_SERVER_TEST_%d: expected parameter \"",
                                     pCheckCommandResponse->index + 1);
                            uAtClientTestPrint(pBytes, length);
                            uPortLog("\" but received \"");
                            uAtClientTestPrint(pReceive, length);
                            uPortLog("\".\n");
                            lastError = 4;
                        }
                    }
                }
                // Finally, after all the parameters, should get the
                // command terminator
                if (lastError == 0) {
                    pBytes = U_AT_CLIENT_TEST_COMMAND_TERMINATOR;
                    length = strlen(pBytes);
                    if ((receiveLength >= length) &&
                        (memcmp(pBytes, pReceive, length) == 0)) {
                        receiveLength -= length;
                        // Should be nothing left
                        if (receiveLength > 0) {
                            lastError = 6;
                        }
                    } else {
                        uPortLog("U_AT_SERVER_TEST_%d: expected terminator (\"",
                                 pCheckCommandResponse->index + 1);
                        uAtClientTestPrint(pBytes, length);
                        uPortLog("\") but received \"");
                        uAtClientTestPrint(pReceive, length);
                        uPortLog("\".\n");
                        lastError = 5;
                    }
                }
            } else {
                uPortLog("U_AT_SERVER_TEST_%d: expected \"%s\""
                         " but received \"",
                         pCheckCommandResponse->index + 1, pBytes);
                uAtClientTestPrint(pReceive, length);
                uPortLog("\".\n");
                lastError = 2;
            }

            pCheckCommandResponse->commandLastError = lastError;
            if (lastError == 0) {
                pCheckCommandResponse->commandPassIndex++;
            } else {
                uPortLog("U_AT_SERVER_TEST_%d: error %d.\n",
                         pCheckCommandResponse->index + 1, lastError);
            }

            if (pCheckCommandResponse->pTestSet[pCheckCommandResponse->index].response.type !=
                U_AT_CLIENT_TEST_RESPONSE_NONE) {
                // To avoid debug prints falling over each other we put the
                // entire response, including URCs if they are to be interleaved,
                // in a malloc()ed buffer, print it, and only then send it to
                // the AT client over the UART.
                pBuffer = (char *) malloc(U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH);
                U_PORT_TEST_ASSERT(pBuffer != NULL);
                pResponse = &(pCheckCommandResponse->pTestSet[pCheckCommandResponse->index].response);
                pUrc = pCheckCommandResponse->pTestSet[pCheckCommandResponse->index].pUrc;
                // Start with a URC line, if there is one
                //lint -esym(613, pBuffer) Suppress possible use of null pointer 'pBuffer': it is checked above
                length = createAtServerResponseUrc(pBuffer,
                                                   U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH, pUrc);
                // Then the initial part of the response
                length += createAtServerResponseStart(pBuffer + length,
                                                      U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH -
                                                      length);
                // Then the URC line again, if there is one
                length += createAtServerResponseUrc(pBuffer + length,
                                                    U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH -
                                                    length, pUrc);
                // Now each line of the response, with URC between each one
                for (size_t x = 0; x < pResponse->numLines; x++) {
                    length += createAtServerResponseLine(pBuffer + length,
                                                         U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH -
                                                         length, &(pResponse->lines[x]));
                    length += createAtServerResponseUrc(pBuffer + length,
                                                        U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH -
                                                        length, pUrc);
                }
                // Finally, send the end of the response
                length += createAtServerResponseEnd(pBuffer + length,
                                                    U_AT_CLIENT_TEST_SERVER_RESPONSE_LENGTH -
                                                    length, pResponse->type,
                                                    U_AT_CLIENT_TEST_CMX_ERROR_NUMBER);

                // Print what we're gonna send and let it be printed
                uPortLog("U_AT_SERVER_TEST_%d: sending response: \"",
                         pCheckCommandResponse->index + 1);
                uAtClientTestPrint(pBuffer, length);
                uPortLog("\"...\n");
                // Let that print
                uPortTaskBlock(100);
                // Now write the buffer to the UART,
                // inserting random delays for extra interest
                increment = length / 10;
                if (increment == 0) {
                    increment = 1;
                }
                pTmp = pBuffer;
                for (size_t x = 0; x < length; x += increment) {
                    uPortUartWrite(uartHandle, pTmp, increment);
                    length -= increment;
                    //lint -e{613} Suppress possible use of null pointer: it is checked
                    pTmp += increment;
                    uPortTaskBlock(rand() % 10);
                }
                if (length > 0) {
                    uPortUartWrite(uartHandle, pTmp, length);
                }
                // Free the buffer again
                free(pBuffer);
            } else {
                uPortLog("U_AT_SERVER_TEST_%d: no response will be sent.\n",
                         pCheckCommandResponse->index + 1);
            }
        }
    }
}

// Callback which echoes what it receives apart from the
// closing command terminator and may interleave this with
// URCs.
// NOTE: don't include "\r\n" in the string to be echoed
// unless it really is a line ending as this is used as a
// cue to send back a URC interleaved between lines.
//lint -e{818} Suppress 'pParameters' could be declared as const:
// need to follow function signature
static void atEchoServerCallback(int32_t uartHandle, uint32_t eventBitmask,
                                 void *pParameters)
{
    int32_t sizeOrError = 0;
    char *pReceive = gAtServerBuffer;
    char *pThis;
    char *pNext;
    size_t length = 0;
    size_t lengthToSend;
    const uAtClientTestResponseLine_t *pUrc = NULL;

    if (pParameters != NULL) {
        pUrc = *((const uAtClientTestResponseLine_t **) pParameters);
    }

    if (eventBitmask & U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED) {
        // Loop until no received characters left to process
        while ((uPortUartGetReceiveSize(uartHandle) > 0) && (sizeOrError >= 0)) {
            sizeOrError = uPortUartRead(uartHandle, pReceive,
                                        sizeof(gAtServerBuffer) -
                                        (pReceive - gAtServerBuffer));
            if (sizeOrError > 0) {
                pReceive += sizeOrError;
                length += sizeOrError;
                if (length >= sizeof(gAtServerBuffer)) {
                    length = 0;
                    sizeOrError = -1;
                }
            }
            // Wait long enough for everything to have been received
            // and for any prints in the sending task to be printed
            uPortTaskBlock(100);
        }

        pThis = gAtServerBuffer;
        if (length > U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES) {
            length -= U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES;
            while (length > 0) {
                // Send back the received string line by line, inserting
                // a URC, if one was given, between each line
                lengthToSend = length;
                pNext = strstr(pThis, U_AT_CLIENT_CRLF);
                if (pNext != NULL) {
                    pNext += U_AT_CLIENT_CRLF_LENGTH_BYTES;
                    lengthToSend = pNext - pThis;
                }
                if (pUrc != NULL) {
                    // Send the URC string between the lines
                    uPortUartWrite(uartHandle,
                                   U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                   strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
                    uPortUartWrite(uartHandle, pUrc->pPrefix, strlen(pUrc->pPrefix));
                    for (size_t x = 0; x < pUrc->numParameters; x++) {
                        if (x > 0) {
                            uPortUartWrite(uartHandle, U_AT_CLIENT_TEST_DELIMITER,
                                           strlen(U_AT_CLIENT_TEST_DELIMITER));
                        }
                        uPortUartWrite(uartHandle, pUrc->parametersRaw[x].pBytes,
                                       pUrc->parametersRaw[x].length);
                    }
                    uPortUartWrite(uartHandle,
                                   U_AT_CLIENT_TEST_RESPONSE_TERMINATOR,
                                   strlen(U_AT_CLIENT_TEST_RESPONSE_TERMINATOR));
                }
                // Now send the line
                uPortUartWrite(uartHandle, pThis, lengthToSend);
                length -= lengthToSend;
                pThis = pNext;
            }
        }
    }
}

// A transmit intercept function.
//lint -e{818} Suppress 'pContext' could be declared as const:
// need to follow function signature
static const char *pInterceptTx(uAtClientHandle_t atHandle,
                                const char **ppData,
                                size_t *pLength,
                                void *pContext)
{
    U_PORT_TEST_ASSERT(atHandle != 0);
    U_PORT_TEST_ASSERT(pLength != NULL);
    //lint -esym(613, pLength) Suppress possible use of null pointer
    U_PORT_TEST_ASSERT((ppData != NULL) || (*pLength == 0));
    U_PORT_TEST_ASSERT(*((char *) pContext) == 'T');

    if (ppData != NULL) {
        // Remember the last ppData we had so
        // that we don't return NULL when the
        // flush call (with a NULL ppData) comes.
        // The return value will be what we got
        // and move ppData on to indicate that we've
        // processed all of the data
        gpInterceptTxDataLast = *ppData;
        *ppData += *pLength;
    }

    return gpInterceptTxDataLast;
}

// A receive intercept function.
//lint -e{818} Suppress 'pContext' could be declared as const:
// need to follow function signature
static char *pInterceptRx(uAtClientHandle_t atHandle,
                          char **ppData, size_t *pLength,
                          void *pContext)
{
    char *pData = NULL;

    U_PORT_TEST_ASSERT(atHandle != 0);
    U_PORT_TEST_ASSERT(pLength != NULL);
    //lint -esym(613, pLength) Suppress possible use of null pointer
    U_PORT_TEST_ASSERT((ppData != NULL) || (*pLength == 0));
    U_PORT_TEST_ASSERT(*((char *) pContext) == 'R');

    if (ppData != NULL) {
        if (*pLength > 0) {
            // Set the return value to what we were given
            // and move ppData on to indicate that we've
            // processed all of the received data
            pData = *ppData;
            *ppData += *pLength;
        } else {
            // Just return NULL to indicate we're done
        }
    }

    return pData;
}

# endif
#endif

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

// Print an AT string, displaying control characters in
// human-readable form.
void uAtClientTestPrint(const char *pBytes, size_t length)
{
    char c;

    if (pBytes != NULL) {
        for (size_t x = 0; x < length; x++) {
            c = *pBytes++;
            if (!isprint((int32_t) c)) {
                if (c == '\r') {
                    uPortLog("\\r");
                } else if (c == '\n') {
                    uPortLog("\\n");
                } else {
                    // Print the hex
                    uPortLog("[%02x]", c);
                }
            } else {
                // Print the ASCII character
                uPortLog("%c", c);
            }
        }
    }
}

// Check that a parameter is as expected.
// Returns zero if it is, else error code.
int32_t uAtClientTestCheckParam(uAtClientHandle_t atClientHandle,
                                const uAtClientTestParameter_t *pParameter,
                                const char *pPostfix)
{
    int32_t lastError = -1;
    int32_t y;
    size_t z;
    int32_t int32;
    uint64_t uint64;
    bool ignoreStopTag = false;
    bool standalone = false;
    char *pBuffer;

#if !U_CFG_ENABLE_LOGGING
    (void) pPostfix;
#endif

    pBuffer = (char *) malloc(U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH);
    if (pBuffer != NULL) {
        lastError = 0;
        switch (pParameter->type) {
            case U_AT_CLIENT_TEST_PARAMETER_INT32:
                int32 = uAtClientReadInt(atClientHandle);
                uPortLog("U_AT_CLIENT_TEST%s: read int32_t"
                         " parameter %d (expected %d).\n",
                         pPostfix, int32, pParameter->parameter.int32);
                if (int32 != pParameter->parameter.int32) {
                    lastError = 1;
                }
                break;
            case U_AT_CLIENT_TEST_PARAMETER_UINT64:
                if (uAtClientReadUint64(atClientHandle, &uint64) == 0) {
                    uPortLog("U_AT_CLIENT_TEST%s: read uint64_t"
                             " parameter %u (expected %u, noting that"
                             " this may not print properly where 64-bit"
                             " printf() is not supported).\n",
                             pPostfix, (uint32_t) uint64,
                             (uint32_t) pParameter->parameter.uint64);
                    if (uint64 != pParameter->parameter.uint64) {
                        lastError = 2;
                    }
                } else {
                    uPortLog("U_AT_CLIENT_TEST%s: error reading uint64_t.\n",
                             pPostfix);
                    lastError = 3;
                }
                break;
            case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_STRING_IGNORE_STOP_TAG:
                ignoreStopTag = true;
            // Deliberate fall-through
            //lint -fallthrough
            case U_AT_CLIENT_TEST_PARAMETER_STRING:
                z = U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH;
                if (pParameter->length > 0) {
                    z = pParameter->length;
                    if (z > U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH) {
                        z = U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH;
                    }
                }
                y = uAtClientReadString(atClientHandle, pBuffer, z, ignoreStopTag);
                if (y >= 0) {
                    uPortLog("U_AT_CLIENT_TEST%s: read %d character(s)"
                             " of string parameter \"", pPostfix, y);
                    uAtClientTestPrint(pBuffer, y);
                    uPortLog("\" (expected %d character(s) \"",
                             strlen(pParameter->parameter.pString));
                    uAtClientTestPrint(pParameter->parameter.pString,
                                       strlen(pParameter->parameter.pString));
                    uPortLog("\").\n");
                    // Check length
                    if (y == strlen(pParameter->parameter.pString)) {
                        // Check explicitly for a terminator
                        if (*(pBuffer + y) != 0) {
                            uPortLog("U_AT_CLIENT_TEST%s: string terminator missing.\n",
                                     pPostfix);
                            lastError = 4;
                        } else {
                            if (memcmp(pBuffer, pParameter->parameter.pString, y) != 0) {
                                uPortLog("U_AT_CLIENT_TEST%s: compare failed.\n",
                                         pPostfix);
                                lastError = 5;
                            }
                        }
                    } else {
                        lastError = 6;
                    }
                } else {
                    uPortLog("U_AT_CLIENT_TEST%s: error reading string.\n",
                             pPostfix);
                    lastError = 7;
                }
                break;
            case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_BYTES_IGNORE_STOP_TAG:
                uAtClientIgnoreStopTag(atClientHandle);
            // Deliberate fall-through
            //lint -fallthrough
            case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_BYTES_STANDALONE:
                standalone = true;
            // Deliberate fall-through
            //lint -fallthrough
            case U_AT_CLIENT_TEST_PARAMETER_BYTES:
                z = pParameter->length;
                if (z > U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH) {
                    z = U_AT_CLIENT_TEST_RESPONSE_BUFFER_LENGTH;
                }
                y = uAtClientReadBytes(atClientHandle, pBuffer, z, standalone);
                if (y >= 0) {
                    uPortLog("U_AT_CLIENT_TEST%s: read %d byte(s)"
                             " (expected %d byte(s)).\n", pPostfix, y,
                             pParameter->length);
                    if (y != pParameter->length) {
                        uPortLog("U_AT_CLIENT_TEST%s: lengths differ.\n", pPostfix);
                        lastError = 8;
                    } else {
                        if (memcmp(pBuffer, pParameter->parameter.pBytes, y) != 0) {
                            uPortLog("U_AT_CLIENT_TEST%s: compare failed.\n",
                                     pPostfix);
                            lastError = 9;
                        }
                    }
                } else {
                    uPortLog("U_AT_CLIENT_TEST%s: error reading byte(s).",
                             pPostfix);
                    lastError = 10;
                }
                break;
            case U_AT_CLIENT_TEST_PARAMETER_NONE:
            case U_AT_CLIENT_TEST_PARAMETER_COMMAND_QUOTED_STRING:
            case U_AT_CLIENT_TEST_PARAMETER_COMMAND_BYTES_STANDALONE:
            default:
                uPortLog("U_AT_CLIENT_TEST%s: unhandled check parameter"
                         " type (%d).\n", pPostfix, pParameter->type);
                lastError = -1;
                break;
        }
    }

    // Free the buffer again
    free(pBuffer);

    return lastError;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS: TESTS
 * -------------------------------------------------------------- */

/** Basic test: initialise and then de-initialise the AT client.
 *
 * IMPORTANT: see notes in u_cfg_test_platform_specific.h for the
 * naming rules that must be followed when using the
 * U_PORT_TEST_FUNCTION() macro.
 */
U_PORT_TEST_FUNCTION("[atClient]", "atClientInitialisation")
{
    U_PORT_TEST_ASSERT(uPortInit() == 0);
    U_PORT_TEST_ASSERT(uAtClientInit() == 0);
    uAtClientDeinit();
    uPortDeinit();
}

#if (U_CFG_TEST_UART_A >= 0)
/** Add an AT client then try getting and setting all of the
 * configuration items.  Requires one UART with no
 * particular wiring.
 */
U_PORT_TEST_FUNCTION("[atClient]", "atClientConfiguration")
{
    uAtClientHandle_t atClientHandle;
    bool thingIsOn;
    int32_t x;
    char c;
    int32_t heapUsed;
    int32_t heapClibLossOffset = (int32_t) gSystemHeapLost;

    // Whatever called us likely initialised the
    // port so deinitialise it here to obtain the
    // correct initial heap size
    uPortDeinit();
    heapUsed = uPortGetHeapFree();
    U_PORT_TEST_ASSERT(uPortInit() == 0);
    gUartAHandle = uPortUartOpen(U_CFG_TEST_UART_A,
                                 U_CFG_TEST_BAUD_RATE,
                                 NULL,
                                 U_CFG_TEST_UART_BUFFER_LENGTH_BYTES,
                                 U_CFG_TEST_PIN_UART_A_TXD,
                                 U_CFG_TEST_PIN_UART_A_RXD,
                                 U_CFG_TEST_PIN_UART_A_CTS,
                                 U_CFG_TEST_PIN_UART_A_RTS);
    U_PORT_TEST_ASSERT(gUartAHandle >= 0);

    U_PORT_TEST_ASSERT(uAtClientInit() == 0);

    uPortLog("U_AT_CLIENT_TEST: adding an AT client on UART %d...\n",
             U_CFG_TEST_UART_A);
    atClientHandle = uAtClientAdd(gUartAHandle, U_AT_CLIENT_STREAM_TYPE_UART,
                                  NULL, U_AT_CLIENT_TEST_AT_BUFFER_LENGTH_BYTES);
    U_PORT_TEST_ASSERT(atClientHandle != NULL);

    thingIsOn = uAtClientDebugGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: debug is %s.\n",
             thingIsOn ? "on" : "off");
    U_PORT_TEST_ASSERT(!thingIsOn);

    thingIsOn = !thingIsOn;
    uAtClientDebugSet(atClientHandle, thingIsOn);
    thingIsOn = uAtClientDebugGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: debug is now %s.\n",
             thingIsOn ? "on" : "off");
    U_PORT_TEST_ASSERT(thingIsOn);

    thingIsOn = uAtClientPrintAtGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: print AT is %s.\n",
             thingIsOn ? "on" : "off");
    U_PORT_TEST_ASSERT(!thingIsOn);

    thingIsOn = !thingIsOn;
    uAtClientPrintAtSet(atClientHandle, thingIsOn);
    thingIsOn = uAtClientPrintAtGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: print AT is now %s.\n",
             thingIsOn ? "on" : "off");
    U_PORT_TEST_ASSERT(thingIsOn);

    x = uAtClientTimeoutGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: timeout is %d ms.\n", x);
    U_PORT_TEST_ASSERT(x == U_AT_CLIENT_DEFAULT_TIMEOUT_MS);

    x++;
    uAtClientTimeoutSet(atClientHandle, x);
    x = uAtClientTimeoutGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: timeout is now %d ms.\n", x);
    U_PORT_TEST_ASSERT(x == U_AT_CLIENT_DEFAULT_TIMEOUT_MS + 1);

    c = uAtClientDelimiterGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: delimiter is '%c'.\n", c);
    U_PORT_TEST_ASSERT(c == U_AT_CLIENT_DEFAULT_DELIMITER);

    c = 'a';
    uAtClientDelimiterSet(atClientHandle, c);
    c = uAtClientDelimiterGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: delimiter is now '%c'.\n", c);
    U_PORT_TEST_ASSERT(c == 'a');

    x = uAtClientDelayGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: delay is %d ms.\n", x);
    U_PORT_TEST_ASSERT(x == U_AT_CLIENT_DEFAULT_DELAY_MS);

    x++;
    uAtClientDelaySet(atClientHandle, x);
    x = uAtClientDelayGet(atClientHandle);
    uPortLog("U_AT_CLIENT_TEST: delay is now %d ms.\n", x);
    U_PORT_TEST_ASSERT(x == U_AT_CLIENT_DEFAULT_DELAY_MS + 1);

    // Can't do much with this other than set it
    uPortLog("U_AT_CLIENT_TEST: setting consecutive AT"
             " timeout callback...\n");
    uAtClientTimeoutCallbackSet(atClientHandle,
                                consecutiveTimeoutCallback);

    // Check the stack extents for the URC and callbacks tasks
    checkStackExtents(atClientHandle);

    uPortLog("U_AT_CLIENT_TEST: removing AT client...\n");
    uAtClientRemove(atClientHandle);
    uAtClientDeinit();

    uPortUartClose(gUartAHandle);
    gUartAHandle = -1;
    uPortDeinit();

    // Check for memory leaks
    heapUsed -= uPortGetHeapFree();
    uPortLog("U_AT_CLIENT_TEST: %d byte(s) of heap were lost to"
             " the C library during this test and we have"
             " leaked %d byte(s).\n",
             gSystemHeapLost - heapClibLossOffset,
             heapUsed - (gSystemHeapLost - heapClibLossOffset));
    // heapUsed < 0 for the Zephyr case where the heap can look
    // like it increases (negative leak)
    U_PORT_TEST_ASSERT((heapUsed < 0) ||
                       (heapUsed <= ((int32_t) gSystemHeapLost) - heapClibLossOffset));
}

# if (U_CFG_TEST_UART_B >= 0)
/** Add an AT client, send the test commands of gAtClientTestSet1[],
 * to atServerCallback() over a UART where they are checked and then,
 * the test responses/URCs of gAtClientTestSet1[] are sent back by
 * atServerCallback() to the first UART whereupon the AT client acts
 * upon them and the outcome checked.  Requires two UARTs wired
 * back-to-back.
 */
U_PORT_TEST_FUNCTION("[atClient]", "atClientCommandSet1")
{
    uAtClientHandle_t atClientHandle;
    const uAtClientTestCommandResponse_t *pCommandResponse;
    const uAtClientTestParameter_t *pParameter;
    const uAtClientTestResponseLine_t *pLine;
    const uAtClientTestResponseLine_t *pLastUrc = NULL;
    uAtClientTestCheckCommandResponse_t checkCommandResponse;
    uAtClientTestCheckUrc_t checkUrc;
    size_t x;
    int32_t y;
    int32_t lastError = 0;
    uAtClientDeviceError_t deviceError;
    bool isQuoted;
    bool standalone;
    char buffer[5]; // Enough characters for a 3 digit index as a string
    char t = 'T';
    char r = 'R';
    int32_t heapUsed;
    int32_t heapClibLossOffset = (int32_t) gSystemHeapLost;

    memset(&checkCommandResponse, 0, sizeof(checkCommandResponse));
    checkCommandResponse.pTestSet = gAtClientTestSet1;
    memset(&checkUrc, 0, sizeof(checkUrc));
    checkUrc.pUrc = NULL;

    // Whatever called us likely initialised the
    // port so deinitialise it here to obtain the
    // correct initial heap size
    uPortDeinit();
    heapUsed = uPortGetHeapFree();
    U_PORT_TEST_ASSERT(uPortInit() == 0);

    // Set up everything with the two UARTs
    twoUartsPreamble();

    // Set up an AT server event handler on UART 1,
    // use the same task size/priority as the AT URC handler
    // for the sake of convenience.
    // This event handler receives the output of the AT client,
    // checks it, and then sends back the test responses
    U_PORT_TEST_ASSERT(uPortUartEventCallbackSet(gUartBHandle,
                                                 U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED,
                                                 atServerCallback, &checkCommandResponse,
                                                 U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES,
                                                 U_AT_CLIENT_URC_TASK_PRIORITY) == 0);

    U_PORT_TEST_ASSERT(uAtClientInit() == 0);

    uPortLog("U_AT_CLIENT_TEST: adding an AT client on UART %d...\n",
             U_CFG_TEST_UART_A);
    atClientHandle = uAtClientAdd(gUartAHandle, U_AT_CLIENT_STREAM_TYPE_UART,
                                  NULL, U_AT_CLIENT_TEST_AT_BUFFER_LENGTH_BYTES);
    U_PORT_TEST_ASSERT(atClientHandle != NULL);

    uPortLog("U_AT_CLIENT_TEST: setting consecutive AT timeout callback...\n");
    gConsecutiveTimeout = 0;
    uAtClientTimeoutCallbackSet(atClientHandle,
                                consecutiveTimeoutCallback);

    // Add transmit and receive intercepts, though they don't do much
    uAtClientStreamInterceptTx(atClientHandle, pInterceptTx, (void *) &t);
    uAtClientStreamInterceptRx(atClientHandle, pInterceptRx, (void *) &r);

    uPortLog("U_AT_CLIENT_TEST: %d command(s)/response(s) to execute.\n",
             gAtClientTestSetSize1);
    pCommandResponse = gAtClientTestSet1;
    for (x = 0; (x < gAtClientTestSetSize1) && (lastError == 0); x++) {
        // If a URC is specified, install a handler for it if it is
        // different to the one we already have installed
        if ((pCommandResponse->pUrc != NULL) &&
            (pLastUrc != pCommandResponse->pUrc)) {
            // Removing the previous URC handler shouldn't make a
            // difference (the parameter passed in will be the
            // same for any give prefix and so the one that's already
            // there should be fine); randomly decide whether
            // to remove it or not as an additional test
            if ((pLastUrc != NULL) && ((x & 1) == 0)) {
                uAtClientRemoveUrcHandler(atClientHandle, pLastUrc->pPrefix);
            }
            checkUrc.pUrc = pCommandResponse->pUrc;
            lastError = uAtClientSetUrcHandler(atClientHandle,
                                               pCommandResponse->pUrc->pPrefix,
                                               urcHandler, (void *) &checkUrc);
        }
        if (lastError == 0) {
            snprintf(buffer, sizeof(buffer), "_%d", x + 1);
            uPortLog("U_AT_CLIENT_TEST_%d: sending command: \"%s\"...\n",
                     x + 1, pCommandResponse->command.pString);
            uAtClientLock(atClientHandle);
            uAtClientCommandStart(atClientHandle,
                                  pCommandResponse->command.pString);
            for (size_t p = 0;
                 p < pCommandResponse->command.numParameters; p++) {
                pParameter = &(pCommandResponse->command.parameters[p]);
                isQuoted = false;
                standalone = false;
                switch (pParameter->type) {
                    case U_AT_CLIENT_TEST_PARAMETER_INT32:
                        uPortLog("U_AT_CLIENT_TEST_%d: writing int32_t"
                                 " parameter %d...\n", x + 1,
                                 pParameter->parameter.int32);
                        uAtClientWriteInt(atClientHandle,
                                          pParameter->parameter.int32);
                        break;
                    case U_AT_CLIENT_TEST_PARAMETER_UINT64:
                        uPortLog("U_AT_CLIENT_TEST_%d: writing uint64_t"
                                 " parameter %u, noting that this may not"
                                 " print properly where 64-bit printf()"
                                 " is not supported...\n", x + 1,
                                 (uint32_t) (pParameter->parameter.uint64));
                        uAtClientWriteUint64(atClientHandle,
                                             pParameter->parameter.uint64);
                        break;
                    case U_AT_CLIENT_TEST_PARAMETER_COMMAND_QUOTED_STRING:
                        isQuoted = true;
                    // Deliberate fall-through
                    //lint -fallthrough
                    case U_AT_CLIENT_TEST_PARAMETER_STRING:
                        uPortLog("U_AT_CLIENT_TEST_%d: writing string"
                                 " parameter \"%s\"...\n", x + 1,
                                 pParameter->parameter.pString);
                        uAtClientWriteString(atClientHandle,
                                             pParameter->parameter.pString,
                                             isQuoted);
                        break;
                    case U_AT_CLIENT_TEST_PARAMETER_COMMAND_BYTES_STANDALONE:
                        standalone = true;
                    // Deliberate fall-through
                    //lint -fallthrough
                    case U_AT_CLIENT_TEST_PARAMETER_BYTES:
                        uPortLog("U_AT_CLIENT_TEST_%d: writing %d binary"
                                 " byte(s)...\n", x + 1, pParameter->length);
                        uAtClientWriteBytes(atClientHandle,
                                            pParameter->parameter.pBytes,
                                            pParameter->length,
                                            standalone);
                        break;
                    case U_AT_CLIENT_TEST_PARAMETER_NONE:
                    case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_STRING_IGNORE_STOP_TAG:
                    case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_BYTES_IGNORE_STOP_TAG:
                    case U_AT_CLIENT_TEST_PARAMETER_RESPONSE_BYTES_STANDALONE:
                    default:
                        //lint -e(774) suppress always true
                        U_PORT_TEST_ASSERT(false);
                        break;
                }
            }

            // Handle the response
            if (pCommandResponse->response.numLines > 0) {
                // Stop the command part
                uAtClientCommandStop(atClientHandle);
                for (size_t l = 0; l < pCommandResponse->response.numLines; l++) {
                    pLine = &(pCommandResponse->response.lines[l]);
                    uAtClientResponseStart(atClientHandle, pLine->pPrefix);
                    uPortLog("U_AT_CLIENT_TEST_%d: waiting for line %d...\n",
                             x + 1, l + 1);
                    for (size_t p = 0; p < pLine->numParameters; p++) {
                        lastError = uAtClientTestCheckParam(atClientHandle,
                                                            &(pLine->parameters[p]),
                                                            buffer);
                    }
                }
                uAtClientResponseStop(atClientHandle);
            } else {
                uAtClientCommandStopReadResponse(atClientHandle);
            }

            y = uAtClientUnlock(atClientHandle);
            switch (pCommandResponse->response.type) {
                case U_AT_CLIENT_TEST_RESPONSE_OK:
                    if (y == 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command completed"
                                 " successfully.\n", x + 1);
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command failed,"
                                 " return value (%d).\n", x + 1, y);
                        lastError = 11;
                    }
                    break;
                case U_AT_CLIENT_TEST_RESPONSE_NONE:
                    if (y < 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned error"
                                 " (%d) as expected.\n", x + 1, y);
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " success (%d) when it should have timed out.\n",
                                 x + 1, y);
                        lastError = 12;
                    }
                    break;
                case U_AT_CLIENT_TEST_RESPONSE_ERROR:
                    if (y < 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " error (%d) as expected.\n", x + 1, y);
                        uAtClientDeviceErrorGet(atClientHandle, &deviceError);
                        if (deviceError.type != U_AT_CLIENT_DEVICE_ERROR_TYPE_ERROR) {
                            uPortLog("U_AT_CLIENT_TEST_%d: but device"
                                     " error type was %d not %d (ERROR) as expected.\n",
                                     x + 1, deviceError.type,
                                     U_AT_CLIENT_DEVICE_ERROR_TYPE_ERROR);
                            lastError = 13;
                        }
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " success (%d) when it should have returned ERROR.\n",
                                 x + 1);
                        lastError = 14;
                    }
                    break;
                case U_AT_CLIENT_TEST_RESPONSE_CME_ERROR:
                    if (y < 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " error (%d) as expected.\n", x + 1, y);
                        uAtClientDeviceErrorGet(atClientHandle, &deviceError);
                        if (deviceError.type == U_AT_CLIENT_DEVICE_ERROR_TYPE_CME) {
                            if (deviceError.code != U_AT_CLIENT_TEST_CMX_ERROR_NUMBER) {
                                uPortLog("U_AT_CLIENT_TEST_%d: but CME ERROR number"
                                         " was %d not %d as expected.\n",
                                         x + 1, deviceError.code,
                                         U_AT_CLIENT_TEST_CMX_ERROR_NUMBER);
                                lastError = 15;
                            }
                        } else {
                            uPortLog("U_AT_CLIENT_TEST_%d: but device error"
                                     " type was %d not %d (CME ERROR) as expected.\n",
                                     x + 1, deviceError.type,
                                     U_AT_CLIENT_DEVICE_ERROR_TYPE_CME);
                            lastError = 16;
                        }
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " success (%d) when it should have returned CME ERROR.\n",
                                 x + 1);
                        lastError = 17;
                    }
                    break;
                case U_AT_CLIENT_TEST_RESPONSE_CMS_ERROR:
                    if (y < 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " error (%d) as expected.\n", x + 1, y);
                        uAtClientDeviceErrorGet(atClientHandle, &deviceError);
                        if (deviceError.type == U_AT_CLIENT_DEVICE_ERROR_TYPE_CMS) {
                            if (deviceError.code != U_AT_CLIENT_TEST_CMX_ERROR_NUMBER) {
                                uPortLog("U_AT_CLIENT_TEST_%d: but CMS ERROR number"
                                         " was %d not %d as expected.\n",
                                         x + 1, deviceError.code,
                                         U_AT_CLIENT_TEST_CMX_ERROR_NUMBER);
                                lastError = 18;
                            }
                        } else {
                            uPortLog("U_AT_CLIENT_TEST_%d: but device error"
                                     " type was %d not %d (CMS ERROR) as expected.\n",
                                     x + 1, deviceError.type,
                                     U_AT_CLIENT_DEVICE_ERROR_TYPE_CMS);
                            lastError = 19;
                        }
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " success (%d) when it should have returned CMS ERROR.\n",
                                 x + 1);
                        lastError = 20;
                    }
                    break;
                case U_AT_CLIENT_TEST_RESPONSE_ABORTED:
                    if (y < 0) {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " error (%d) as expected.\n", x + 1, y);
                        uAtClientDeviceErrorGet(atClientHandle, &deviceError);
                        if (deviceError.type != U_AT_CLIENT_DEVICE_ERROR_TYPE_ABORTED) {
                            uPortLog("U_AT_CLIENT_TEST_%d: but device"
                                     " error type was %d not %d (ABORTED) as expected.\n",
                                     x + 1, deviceError.type,
                                     U_AT_CLIENT_DEVICE_ERROR_TYPE_ABORTED);
                            lastError = 13;
                        }
                    } else {
                        uPortLog("U_AT_CLIENT_TEST_%d: command returned"
                                 " success (%d) when it should have returned ABORTED.\n",
                                 x + 1);
                        lastError = 14;
                    }
                    break;
                default:
                    uPortLog("U_AT_CLIENT_TEST_%d: unknown response"
                             " type (%d).\n", x + 1, pCommandResponse->response.type);
                    lastError = -1;
                    break;
            }
        }

        checkCommandResponse.responseLastError = lastError;
        if (lastError == 0) {
            // If we've got here then this response passes
            checkCommandResponse.responsePassIndex++;
            // Reflect any URC error into lastError`
            if (checkUrc.lastError != 0) {
                lastError = checkUrc.lastError;
            }
        }

        // Next one
        pLastUrc = pCommandResponse->pUrc;
        pCommandResponse++;
        checkCommandResponse.index++;
    }

    uPortLog("U_AT_CLIENT_TEST: at end of test %d out of %d,"
             " %d command(s) passed, %d response(s) passed"
             " and, of %d URCs (%d expected), %d passed.\n",
             x, gAtClientTestSetSize1,
             checkCommandResponse.commandPassIndex,
             checkCommandResponse.responsePassIndex,
             checkUrc.count, U_AT_CLIENT_TEST_NUM_URCS_SET_1,
             checkUrc.passIndex);
    if (checkCommandResponse.commandLastError != 0) {
        uPortLog("U_AT_CLIENT_TEST: command error was %d"
                 " (check the test code to find out what"
                 " this means).\n",
                 checkCommandResponse.commandLastError);
    }
    if (checkCommandResponse.responseLastError != 0) {
        uPortLog("U_AT_CLIENT_TEST: response error was %d"
                 " (check the test code to find out what"
                 " this means).\n",
                 checkCommandResponse.responseLastError);
    }
    if (checkUrc.lastError != 0) {
        uPortLog("U_AT_CLIENT_TEST: URC error was %d"
                 " (check the test code to find out what"
                 " this means).\n",
                 checkUrc.lastError);
    }

    // Check the stack extents for the URC and callbacks tasks
    checkStackExtents(atClientHandle);

    uPortLog("U_AT_CLIENT_TEST: removing AT client...\n");
    uAtClientRemove(atClientHandle);
    uAtClientDeinit();

    uPortUartClose(gUartBHandle);
    gUartBHandle = -1;
    uPortUartClose(gUartAHandle);
    gUartAHandle = -1;
    uPortDeinit();

    // Fail the test if an error occurred: doing this here
    // rather than asserting above so that clean-up happens
    // and hence we don't end up with mutexes left locked
    U_PORT_TEST_ASSERT(checkCommandResponse.commandPassIndex == x);
    U_PORT_TEST_ASSERT(checkCommandResponse.responsePassIndex == x);
    U_PORT_TEST_ASSERT(checkCommandResponse.commandLastError == 0);
    U_PORT_TEST_ASSERT(checkCommandResponse.responseLastError == 0);
    U_PORT_TEST_ASSERT(checkUrc.count == U_AT_CLIENT_TEST_NUM_URCS_SET_1);
    U_PORT_TEST_ASSERT(checkUrc.passIndex == U_AT_CLIENT_TEST_NUM_URCS_SET_1);
    U_PORT_TEST_ASSERT(gConsecutiveTimeout == 0);

#ifndef __XTENSA__
    // Check for memory leaks
    // TODO: this if'ed out for ESP32 (xtensa compiler) at
    // the moment as there is an issue with ESP32 hanging
    // on to memory in the UART drivers that can't easily be
    // accounted for.
    heapUsed -= uPortGetHeapFree();
    uPortLog("U_AT_CLIENT_TEST: %d byte(s) of heap were lost to"
             " the C library during this test and we have"
             " leaked %d byte(s).\n",
             gSystemHeapLost - heapClibLossOffset,
             heapUsed - (gSystemHeapLost - heapClibLossOffset));
    // heapUsed < 0 for the Zephyr case where the heap can look
    // like it increases (negative leak)
    U_PORT_TEST_ASSERT((heapUsed < 0) ||
                       (heapUsed <= ((int32_t) gSystemHeapLost) - heapClibLossOffset));
#else
    (void) heapUsed;
    (void) heapClibLossOffset;
#endif
}

/** Add an AT client and use an AT echo responder to bounce-back
 * to us the the test responses/URCs of gAtClientTestSet2[] where
 * they are acted up on by the AT client and the outcome checked.
 * Requires two UARTs wired back-to-back.
 */
U_PORT_TEST_FUNCTION("[atClient]", "atClientCommandSet2")
{
    uAtClientHandle_t atClientHandle;
    const uAtClientTestEcho_t *pEcho;
    uAtClientTestCheckUrc_t checkUrc;
    const uAtClientTestResponseLine_t **ppUrc = &(checkUrc.pUrc);
    const char *pUrcPrefix;
    size_t x = 0;
    int32_t lastError = -1;
    int32_t y;
    int32_t heapUsed;
    int32_t heapClibLossOffset = (int32_t) gSystemHeapLost;

    memset(&checkUrc, 0, sizeof(checkUrc));
    checkUrc.pUrc = NULL;

    // Whatever called us likely initialised the
    // port so deinitialise it here to obtain the
    // correct initial heap size
    uPortDeinit();
    heapUsed = uPortGetHeapFree();
    U_PORT_TEST_ASSERT(uPortInit() == 0);

    // Set up everything with the two UARTs
    twoUartsPreamble();

    // Set up an AT echo responder on UART 1.
    // This event responder receives the output of the AT client and
    // echoes back all except the command terminator on the end
    U_PORT_TEST_ASSERT(uPortUartEventCallbackSet(gUartBHandle,
                                                 U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED,
                                                 atEchoServerCallback, (void *) ppUrc,
                                                 U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES,
                                                 U_AT_CLIENT_URC_TASK_PRIORITY) == 0);

    U_PORT_TEST_ASSERT(uAtClientInit() == 0);

    uPortLog("U_AT_CLIENT_TEST: adding an AT client on UART %d...\n",
             U_CFG_TEST_UART_A);
    atClientHandle = uAtClientAdd(gUartAHandle, U_AT_CLIENT_STREAM_TYPE_UART,
                                  NULL, U_AT_CLIENT_TEST_AT_BUFFER_LENGTH_BYTES);
    U_PORT_TEST_ASSERT(atClientHandle != NULL);

    uPortLog("U_AT_CLIENT_TEST: setting consecutive AT timeout callback...\n");
    gConsecutiveTimeout = 0;
    uAtClientTimeoutCallbackSet(atClientHandle,
                                consecutiveTimeoutCallback);

    // First, set an AT timeout and check that it is obeyed
    uAtClientTimeoutSet(atClientHandle, U_AT_CLIENT_TEST_AT_TIMEOUT_MS);
    uPortLog("U_AT_CLIENT_TEST: setting and checking AT timeout..\n");
    if (atTimeoutIsObeyed(atClientHandle, U_AT_CLIENT_TEST_AT_TIMEOUT_MS)) {
        // Send out a boring thing that will be echoed
        // back to us, just to be sure everything is working
        uAtClientLock(atClientHandle);
        uAtClientCommandStart(atClientHandle, "\r\nOK\r\n");
        uAtClientCommandStop(atClientHandle);
        uAtClientResponseStart(atClientHandle, NULL);
        uAtClientResponseStop(atClientHandle);
        lastError = uAtClientUnlock(atClientHandle);
        if (lastError != 0) {
            uPortLog("U_AT_CLIENT_TEST: can't even get \"OK\""
                     "back! (error %d).\n", lastError);
        }

        // Now go through the list of response strings
        // and the functions to handle them
        for (x = 0; (x < gAtClientTestSetSize2) && (lastError == 0); x++) {
            pEcho = &(gAtClientTestSet2[x]);
            // If a URC is specified, install a handler for it
            pUrcPrefix = NULL;
            checkUrc.pUrc = pEcho->pUrc;
            if (pEcho->pUrc != NULL) {
                pUrcPrefix = pEcho->pUrc->pPrefix;
            }
            if (pUrcPrefix != NULL) {
                lastError = uAtClientSetUrcHandler(atClientHandle,
                                                   pUrcPrefix,
                                                   urcHandler,
                                                   (void *) &checkUrc);
            }

            if (lastError == 0) {
                // Lock the AT stream and send the string to be echoed
                uAtClientLock(atClientHandle);
                // Since the echoable string may contain NULLs (e.g. for a
                // "bytes" parameter) we put a NULL string in uAtClientCommandStart()
                // and then send the whole echoable string directly to the UART
                uPortLog("U_AT_CLIENT_TEST_%d: sending out string to be echoed: \"",
                         x + 1);
                uAtClientTestPrint(pEcho->pBytes, pEcho->length);
                uPortLog("\"...\n");
                if (pUrcPrefix != NULL) {
                    uPortLog("U_AT_CLIENT_TEST_%d: ...the URC \"%s\" with %d"
                             " parameter(s) will be interleaved multiple times"
                             " though.\n", x + 1, pUrcPrefix,
                             checkUrc.pUrc->numParameters);
                }
                uAtClientCommandStart(atClientHandle, NULL);
                uPortUartWrite(gUartAHandle, pEcho->pBytes, pEcho->length);
                uAtClientCommandStop(atClientHandle);
                // The part from uAtClientResponseStart() to
                // uAtClientResponseStop() is handled by pFunction
                lastError = pEcho->pFunction(atClientHandle, x, pEcho->pParameters);
                // Unlock the AT stream
                y = uAtClientUnlock(atClientHandle);
                if (y != pEcho->unlockErrorCode) {
                    uPortLog("U_AT_CLIENT_TEST_%d: unlock returned %d"
                             " when %d was expected.\n", x + 1, y,
                             pEcho->unlockErrorCode);
                    lastError = -2;
                }
                // Give any URCs on the end of the response time to arrive
                uPortTaskBlock(100);
                // Check for URC errors
                if ((checkUrc.pUrc != NULL) && (checkUrc.lastError != 0)) {
                    lastError = checkUrc.lastError;
                }
            }
        }
    }

    if (lastError == 0) {
        // One of the above tests changes the AT timeout
        // between uAtClientLock() and uAtClientUnlock().
        // This should not have modified the AT timeout
        // outside the locks.  Check here that the timeout
        // we set above is still obeyed.
        uPortLog("U_AT_CLIENT_TEST: checking AT timeout again..\n");
        if (!atTimeoutIsObeyed(atClientHandle,
                               U_AT_CLIENT_TEST_AT_TIMEOUT_MS)) {
            lastError = -2;
        }
    }

    uPortLog("U_AT_CLIENT_TEST: %d out of %d, tests passed and,"
             " of %d URCs (%d expected) %d arrived correctly.\n",
             x, gAtClientTestSetSize2, checkUrc.count,
             U_AT_CLIENT_TEST_NUM_URCS_SET_2, checkUrc.passIndex);

    // Check the stack extents for the URC and callbacks tasks
    checkStackExtents(atClientHandle);

    uPortLog("U_AT_CLIENT_TEST: removing AT client...\n");
    uAtClientRemove(atClientHandle);
    uAtClientDeinit();

    uPortUartClose(gUartBHandle);
    gUartBHandle = -1;
    uPortUartClose(gUartAHandle);
    gUartAHandle = -1;
    uPortDeinit();

    // Fail the test if an error occurred: doing this here
    // rather than asserting above so that clean-up happens
    // and hence we don't end up with mutexes left locked
    U_PORT_TEST_ASSERT(lastError == 0);
    U_PORT_TEST_ASSERT(checkUrc.count == U_AT_CLIENT_TEST_NUM_URCS_SET_2);
    U_PORT_TEST_ASSERT(checkUrc.passIndex == U_AT_CLIENT_TEST_NUM_URCS_SET_2);

    // Check for memory leaks
    heapUsed -= uPortGetHeapFree();
    uPortLog("U_AT_CLIENT_TEST: %d byte(s) of heap were lost to"
             " the C library during this test and we have"
             " leaked %d byte(s).\n",
             gSystemHeapLost - heapClibLossOffset,
             heapUsed - (gSystemHeapLost - heapClibLossOffset));
    // heapUsed < 0 for the Zephyr case where the heap can look
    // like it increases (negative leak)
    U_PORT_TEST_ASSERT((heapUsed < 0) ||
                       (heapUsed <= ((int32_t) gSystemHeapLost) - heapClibLossOffset));
}

# endif
#endif

/** Clean-up to be run at the end of this round of tests, just
 * in case there were test failures which would have resulted
 * in the deinitialisation being skipped.
 */
U_PORT_TEST_FUNCTION("[atClient]", "atClientCleanUp")
{
    int32_t x;

    uAtClientDeinit();
    if (gUartAHandle >= 0) {
        uPortUartClose(gUartAHandle);
    }
    if (gUartBHandle >= 0) {
        uPortUartClose(gUartBHandle);
    }

    x = uPortTaskStackMinFree(NULL);
    if (x != (int32_t) U_ERROR_COMMON_NOT_SUPPORTED) {
        uPortLog("U_AT_CLIENT_TEST: main task stack had a minimum of %d"
                 " byte(s) free at the end of these tests.\n", x);
        U_PORT_TEST_ASSERT(x >= U_CFG_TEST_OS_MAIN_TASK_MIN_FREE_STACK_BYTES);
    }

    uPortDeinit();

    x = uPortGetHeapMinFree();
    if (x >= 0) {
        uPortLog("U_AT_CLIENT_TEST: heap had a minimum of %d"
                 " byte(s) free at the end of these tests.\n", x);
        U_PORT_TEST_ASSERT(x >= U_CFG_TEST_HEAP_MIN_FREE_BYTES);
    }
}

// End of file
