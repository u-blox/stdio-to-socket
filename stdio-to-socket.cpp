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

// Things to help with parsing filenames.
#define DIR_SEPARATORS "\\/"
#define EXT_SEPARATOR "."

// The maximum length of the handshake strings
#define MAX_LEN_HANDSHAKE_STRING 512

// Print the usage text
static void printUsage(char *exeName) {
    fprintf(stderr, "\n%s: run a command and redirect stdout from the command to a TCP socket.\n", exeName);
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  %s command host port -c <string_command> -h <string_host>\n\n", exeName);
    fprintf(stderr, "where:\n");
    fprintf(stderr, "  - command is the command-line to run (ensure that quotes are used if the command contains spaces),\n");
    fprintf(stderr, "  - host is the host computer for the socket (e.g. 127.0.0.1 for this computer),\n");
    fprintf(stderr, "  - port is the port number for the socked (e.g. 5000),\n");
    fprintf(stderr, "  - -c and -h optionally specify a command and host handshake string (up to %d bytes) pair;\n", MAX_LEN_HANDSHAKE_STRING);
    fprintf(stderr, "    if one is specified then both must be specified, for an explanation see below.\n\n");
    fprintf(stderr, "If -c and -h are not specified then any stdout from the command will be sent to the TCP socket.\n");
    fprintf(stderr, "If -c and -h are specified then the behaviour is as follows:\n");
    fprintf(stderr, "  - stdout from the command-line is sent to the TCP socket unless stdout matches string_command,\n");
    fprintf(stderr, "  - if stdout matches string_command then stdout is buffered until string_host is received from\n");
    fprintf(stderr, "    the host on the TCP socket,\n");
    fprintf(stderr, "  - once string_host is received, sending of stdout to the TCP socket is resumed from the point\n");
    fprintf(stderr, "    it left off (i.e. including the string which matched string_command and any other buffered strings),\n");
    fprintf(stderr, "For example:\n");
    fprintf(stderr, "  %s \"C:\\Program Files (x86)\\SEGGER\\JLink_V632a\\JLinkSWOViewerCL.exe -device NRF52832_XXAA\" 127.0.0.1 5000 -c \"ready now\" -h start\n\n", exeName);
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

    *(buf + len) = 0;
    std::cout << buf;

    while (success && (sent < len)) {
        x = send(sock, buf + sent, len - sent, 0);
        success = (x != SOCKET_ERROR);
        if (success) {
            sent += x;
        }
    }

    return sent;
}

// Run the redirection
static bool run(char *commandLine, SOCKET sock, char *commandString, char *hostString)
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
    DWORD socketReadBytes;
    DWORD x = 0;
    size_t commandStringLen = 0;
    size_t hostStringLen = 0;

    if (commandString != NULL) {
        commandStringLen = strlen(commandString);
    }

    if (hostString != NULL) {
        hostStringLen = strlen(hostString);
    }

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
                    // Check for a match with commandString
                    if (commandStringLen > 0) {
                        while ((x < bufBytes) &&
                            (strncmp(buf + x, commandString, bufBytes - x) != 0)) {
                            x++;
                        }
                    } else {
                        x = bufBytes;
                    }
                    // Write the bytes that don't match
                    if (writeOutput(sock, buf, x) != x) {
                        success = false;
                        fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
                    }
                    bufBytes -= x;
                    // If there's a match and we've had at least as many
                    // bytes as are in commandString then we have a full match
                    if (success && (commandStringLen > 0) && (x >= commandStringLen)) {
                        matchBufBytes = 0;
                        // Now wait for the handshake string from the host
                        while (success && (matchBufBytes < hostStringLen)) {
                            socketReadBytes = recv(sock, matchBuf + matchBufBytes, sizeof(matchBuf) - matchBufBytes, 0);
                            if (socketReadBytes > 0) {
                                matchBufBytes += socketReadBytes;
                                // Check for a match with hostString
                                x = 0;
                                while ((x < matchBufBytes) &&
                                       (strncmp(matchBuf + x, hostString, matchBufBytes - x) != 0)) {
                                    x++;
                                }
                                // Lose the characters that don't match
                                matchBufBytes -= x;
                            } else {
                                success = false;
                                fprintf(stderr, "Socket receive failed with error %d.\n", WSAGetLastError());
                            }
                        }
                        if (success) {
                            // The host handshake string must have arrived, so send what was held back
                            if (writeOutput(sock, buf, bufBytes) != bufBytes) {
                                success = false;
                                fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
                            }
                            bufBytes = 0;
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
    char *commandString = NULL;
    char *hostString = NULL;
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
                if (*argv[x] == '-') {
                    if ((strlen(argv[x]) == 2) && (x + 1 < argc)) {
                        switch (*(argv[x] + 1)) {
                            case 'c':
                                commandString = argv[x + 1];
                                x++;
                            break;
                            case 'h':
                                hostString = argv[x + 1];
                                x++;
                            break;
                            default:
                                success = false;
                            break;
                        }
                    } else {
                        success = false;
                    }
                } else {
                    success = false;
                }
            break;
        }
        x++;
    }

    // If all is good...
    if (success && (commandLine != NULL) && (host != NULL) && (port != NULL) &&
        ((commandString == NULL) == (hostString == NULL))) {
        success = false;
        // Start winsock
        x = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (x == 0) {
            // Connect to the server
            sock = connectSocket(host, port);
            if (sock != INVALID_SOCKET) {
                // Perform the redirection
                success = run(commandLine, sock, commandString, hostString);
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