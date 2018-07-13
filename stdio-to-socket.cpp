/* Copyright (c) 2006-2018 u-blox Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <iostream>
#include <thread>
#include <regex>

// Things to help with parsing filenames.
#define DIR_SEPARATORS "\\/"
#define EXT_SEPARATOR "."

// The maximum length of the handshake strings
#define MAX_LEN_HANDSHAKE_STRING 512

// The handshake string we might receive from the command
#define MBED_HANDSHAKE_COMMAND_STRING "mbedmbedmbedmbedmbedmbedmbedmbed\r\n"

// The handshake string we might receive from the host
#define MBED_HANDSHAKE_HOST_STRING "{{__sync;UUID}}\r\n"

// Print the usage text
static void printUsage(char *exeName) {
    fprintf(stderr, "\n%s: run a command and redirect stdout from the command to a TCP socket.\n", exeName);
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  %s command host port\n\n", exeName);
    fprintf(stderr, "where:\n");
    fprintf(stderr, "  - command is the command-line to run (use quotes if the command contains spaces),\n");
    fprintf(stderr, "  - host is the host computer for the socket (e.g. 127.0.0.1 for this computer),\n");
    fprintf(stderr, "  - port is the port number for the socked (e.g. 5000),\n\n");
    fprintf(stderr, "In addition, a version of mbedhtrun handshaking is employed. That is, if the string\n");
    fprintf(stderr, "\"%s\" is received on stdout from the command then\n", MBED_HANDSHAKE_COMMAND_STRING);
    fprintf(stderr, "capture will stop until the string \"%s\" is received from the host.\n", MBED_HANDSHAKE_HOST_STRING);
    fprintf(stderr, "The string recevied from the host will be echoed back to it before sending of stdout to\n");
    fprintf(stderr, "the TCP socket resumes from where it left off (i.e. including the\n");
    fprintf(stderr, "\"%s\" string).\n\n", MBED_HANDSHAKE_COMMAND_STRING);
    fprintf(stderr, "For example:\n");
    fprintf(stderr, "  %s \"C:\\Program Files (x86)\\SEGGER\\JLink_V632a\\JLinkSWOViewerCL.exe -device NRF52832_XXAA\" 127.0.0.1 5000 -m\n\n", exeName);
}

// Open a socket to the server
static SOCKET connectSocket(char *host, char *port)
{
    SOCKET sock = INVALID_SOCKET;
    struct addrinfo *result = NULL;
    struct addrinfo hints;
    int x;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    x = getaddrinfo(host, port, &hints, &result);
    if (x == 0) {
        // Open a socket
        sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock != INVALID_SOCKET) {
            // Connect to server.
            x = connect(sock, result->ai_addr, (int) result->ai_addrlen);
            if (x == SOCKET_ERROR) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                fprintf(stderr, "Unable to connect to server %s:%s.\n", host, port);
            }
        } else {
            fprintf(stderr, "Unable to open a socket (%d).\n", WSAGetLastError());
        }
    } else {
        fprintf(stderr, "Unable to resolve address %s:%s (%d).\n", host, port, x);
    }

    return sock;
}

// Write to the output
static DWORD writeOutput(SOCKET sock, char *buf, DWORD len)
{
    bool success = true;
    DWORD sent = 0;
    DWORD x;

    fprintf(stdout, "%.*s", len, buf);

    while (success && (sent < len)) {
        x = send(sock, buf + sent, len - sent, 0);
        success = (x != SOCKET_ERROR);
        if (success) {
            sent += x;
        }
    }

    return sent;
}

// Check for MBED_HANDSHAKE_COMMAND_STRING in buf, returning the number
// of unmatched bytes.  In other words if the return
// value is less than len then there has been a partial match while
// if the len - the return value is greater than or equal
// to the length of MBED_HANDSHAKE_COMMAND_STRING then there has been a
// full match
static DWORD checkCommand(char *buf, int len) {
    bool match = false;
    int unmatchedBytes = 0;
    int x;

    while ((unmatchedBytes < len) && !match) {
        x = sizeof(MBED_HANDSHAKE_COMMAND_STRING) - 1;
        if (len - unmatchedBytes < x) {
            x = len - unmatchedBytes;
        }
        if (strncmp(buf + unmatchedBytes, MBED_HANDSHAKE_COMMAND_STRING, x) == 0) {
            match = true;
        } else {
            unmatchedBytes++;
        }
    }

    return unmatchedBytes;
}

// Wait for the {{_sync;UUID}} word from the host
static DWORD waitHost(SOCKET sock, char *matchBuf, int matchBufLen)
{
    bool success = true;
    int socketReadBytes;
    int matchBufBytes = 0;
    int matchBufState = 0;
    int removeBytes = 0;
    int outputBytes = 0;
    bool needMore;

    while (success && (matchBufState < 2)) {
        needMore = false;
        socketReadBytes = recv(sock, matchBuf + matchBufBytes, matchBufLen - matchBufBytes, 0);
        if (socketReadBytes > 0) {
            matchBufBytes += socketReadBytes;
            while ((removeBytes + outputBytes < matchBufBytes) && (matchBufState < 2) && !needMore) {
                switch (matchBufState) {
                    case 0: // Waiting for "{{__sync;"
                        if (strncmp(matchBuf + removeBytes + outputBytes, "{{__sync;", matchBufBytes - removeBytes - outputBytes) == 0) {
                            // If there were enough characters in the buffer
                            // to match all of "{{__sync;" then we can move on
                            if (matchBufBytes - removeBytes - outputBytes >= 9) {
                                matchBufState++;
                                outputBytes += 9;
                            } else {
                                needMore = true;
                            }
                        } else {
                            removeBytes++;
                        }
                    break;
                    case 1: // Waiting for "}}"
                        if (strncmp(matchBuf + removeBytes + outputBytes, "}}", matchBufBytes - removeBytes - outputBytes) == 0) {
                            // If there were enough characters in the buffer
                            // to match all of "}}" then we can move on
                            if (matchBufBytes - removeBytes - outputBytes >= 2) {
                                matchBufState++;
                                outputBytes += 2;
                            } else {
                                needMore = true;
                            }
                        } else {
                            outputBytes++;
                        }
                    break;
                    default:
                        success = false;
                    break;
                }
            }
            // Copy down to overwrite any unwanted characters
            memcpy(matchBuf, matchBuf + removeBytes, matchBufBytes - removeBytes);
            matchBufBytes -= removeBytes;
            removeBytes = 0;
        } else {
            success = false;
            outputBytes = 0;
            fprintf(stderr, "Socket receive failed with error %d.\n", WSAGetLastError());
        }
    }

    return outputBytes;
}

// Run the redirection
static bool run(char *commandLine, SOCKET sock)
{
    bool success = false;
    SECURITY_ATTRIBUTES securityAttributes;
    PROCESS_INFORMATION processInfo;
    STARTUPINFO startupInfo;
    HANDLE pipeChildWrite = NULL;
    HANDLE pipeParentRead = NULL;
    char buf[MAX_LEN_HANDSHAKE_STRING + 1];
    DWORD bufBytes = 0;
    DWORD pipeReadBytes;
    char matchBuf[MAX_LEN_HANDSHAKE_STRING];
    DWORD matchBufBytes = 0;
    DWORD unmatchedBytes = 0;

    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;
    securityAttributes.lpSecurityDescriptor = NULL;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    ZeroMemory(&processInfo, sizeof(processInfo));

    // Create the pipes
    if (CreatePipe(&pipeParentRead, &pipeChildWrite, &securityAttributes, 0) &&
        SetHandleInformation(pipeParentRead, HANDLE_FLAG_INHERIT, 0)) {

        startupInfo.cb = sizeof(startupInfo);
        startupInfo.wShowWindow = SW_SHOW;
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.hStdOutput = pipeChildWrite;
        startupInfo.hStdError = NULL;
        startupInfo.hStdInput = NULL;
        startupInfo.dwFlags |= STARTF_USESTDHANDLES;

        // And create the process
        if (CreateProcess(NULL, commandLine, NULL, NULL, TRUE, 0, NULL, NULL, &startupInfo, &processInfo)) {
            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
            success = true;
            // Read the child's output
            while (success && (success = ReadFile(pipeParentRead, buf + bufBytes, sizeof(buf) - 1 - bufBytes, &pipeReadBytes, NULL))) {
                if (pipeReadBytes > 0) {
                    bufBytes += pipeReadBytes;
                    // Check for a match with MBED_HANDSHAKE_COMMAND_STRING
                    unmatchedBytes = checkCommand(buf, bufBytes);
                    // Write the bytes that don't match
                    if (writeOutput(sock, buf, unmatchedBytes) != unmatchedBytes) {
                        success = false;
                        fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
                    }
                    // Copy down to overwrite any unmatched characters
                    memcpy(buf, buf + unmatchedBytes, bufBytes - unmatchedBytes);
                    bufBytes -= unmatchedBytes;
                    unmatchedBytes = 0;
                    // If we are holding more characterss than MBED_HANDSHAKE_COMMAND_STRING
                    // then we have a match
                    if (success && (bufBytes >= sizeof(MBED_HANDSHAKE_COMMAND_STRING) - 1)) {
                        // Now wait for the handshake string from the host
                        matchBufBytes = waitHost(sock, matchBuf, sizeof (matchBuf));
                        if (matchBufBytes > 0) {
                            // The host handshake string must have arrived, so send what was held back
                            if (writeOutput(sock, buf, bufBytes) != bufBytes) {
                                success = false;
                                fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
                            }
                            bufBytes = 0;
                            unmatchedBytes = 0;
                            // Then echo-back the matched buffer from the host
                            if (writeOutput(sock, matchBuf, matchBufBytes) != matchBufBytes) {
                                success = false;
                                fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
                            }
                        } else {
                            success = false;
                        }
                    }
                }
            }
            TerminateProcess(processInfo.hProcess, 1);
        } else {
            fprintf(stderr, "Unable to execute \"%s\".\n", commandLine);
        }
    }

    return success;
}

int main(int argc, char* argv[])
{
    bool success = true;
    int x = 0;
    char *context = NULL;
    char *exeName = NULL;
    char *commandLine = NULL;
    char *host = NULL;
    char *port = NULL;
    char *argString;
    WSADATA wsaData;
    SOCKET sock;

    // Find the exe name in the first argument
    argString = strtok_s(argv[x], DIR_SEPARATORS, &context);
    while (argString != NULL) {
        exeName = argString;
        argString = strtok_s(NULL, DIR_SEPARATORS, &context);
    }
    if (exeName != NULL) {
        // Remove the extension
        argString = strtok_s(exeName, EXT_SEPARATOR, &context);
        if (argString != NULL) {
            exeName = argString;
        }
    }
    x++;

    // Look for all the command line parameters
    while ((x < argc) && success) {
        switch (x) {
            case 1:
                commandLine = argv[x];
            break;
            case 2:
                host = argv[x];
            break;
            case 3:
                port = argv[x];
            break;
            default:
                success = false;
            break;
        }
        x++;
    }

    // If all is good...
    if (success && (commandLine != NULL) && (host != NULL) && (port != NULL)) {
        success = false;
        // Start winsock
        x = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (x == 0) {
            // Connect to the server
            sock = connectSocket(host, port);
            if (sock != INVALID_SOCKET) {
                // Perform the redirection
                success = run(commandLine, sock);
                shutdown(sock, SD_SEND);
                closesocket(sock);
            }
            WSACleanup();
        } else {
            fprintf(stderr, "Unable to start WinSock (%d).\n", x);
        }
    } else {
        printUsage(exeName);
    }

    return success;
}