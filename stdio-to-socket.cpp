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

// Print the usage text
static void printUsage(char *exeName) {
    fprintf(stderr, "\n%s: run a command and redirect stdout from the command to a TCP socket.\n", exeName);
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  %s command host port\n\n", exeName);
    fprintf(stderr, "where:\n");
    fprintf(stderr, "  - command is the command-line to run (use quotes if the command contains spaces),\n");
    fprintf(stderr, "  - host is the host computer for the socket (e.g. 127.0.0.1 for this computer),\n");
    fprintf(stderr, "  - port is the port number for the socked (e.g. 5000).\n\n");
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

// Run the redirection
static bool run(char *commandLine, SOCKET sock)
{
    bool success = false;
    SECURITY_ATTRIBUTES securityAttributes;
    PROCESS_INFORMATION processInfo;
    STARTUPINFO startupInfo;
    HANDLE pipeChildWrite = NULL;
    HANDLE pipeParentRead = NULL;
    char buf[1024 + 1];
    DWORD bufBytes = 0;
    DWORD pipeReadBytes;

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
            // Read the child's output and write it
            while (success && (success = ReadFile(pipeParentRead, buf, sizeof(buf) - 1, &pipeReadBytes, NULL))) {
                if (pipeReadBytes > 0) {
                    if (writeOutput(sock, buf, pipeReadBytes) != pipeReadBytes) {
                        success = false;
                        fprintf(stderr, "Socket send failed with error %d.\n", WSAGetLastError());
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