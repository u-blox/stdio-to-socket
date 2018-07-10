/*
* This code is based on the code here:
* https://stackoverflow.com/questions/22088234/redirect-the-stdout-of-a-child-process-to-the-parent-process-stdin
*/

#include <Windows.h>
#include <tchar.h>

#include <iostream>
#include <thread>

enum { ParentRead, ParentWrite, ChildWrite, ChildRead, NumPipeTypes };

int main(int /*argc*/, char* /*argv*/[])
{
    bool success;
    SECURITY_ATTRIBUTES sa;
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    char ReadBuff[4096 + 1];
    DWORD ReadNum;
    HANDLE pipes[NumPipeTypes];
    TCHAR cmd[] = _T("\"C:\\Program Files (x86)\\SEGGER\\JLink_V632a\\JLinkSWOViewerCL.exe\" -device NRF52832_XXAA");

    ZeroMemory(&si, sizeof(STARTUPINFO));
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    success = CreatePipe(&pipes[ParentWrite], &pipes[ChildRead], &sa, 0) &&
        CreatePipe(&pipes[ParentRead], &pipes[ChildWrite], &sa, 0);
    if (success) {
        // make sure the handles the parent will use aren't inherited.
        SetHandleInformation(pipes[ParentRead], HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(pipes[ParentWrite], HANDLE_FLAG_INHERIT, 0);

        si.cb = sizeof(STARTUPINFO);
        si.wShowWindow = SW_SHOW;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.hStdOutput = pipes[ChildWrite];
        si.hStdError = pipes[ChildWrite];
        si.hStdInput = pipes[ChildRead];

        success = CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
        if (success) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(pipes[ChildRead]);
            CloseHandle(pipes[ChildWrite]);
            CloseHandle(pipes[ParentWrite]);

            while (success) {
                success = ReadFile(pipes[ParentRead], ReadBuff, sizeof(ReadBuff) - 1, &ReadNum, NULL);
                if (ReadNum > 0) {
                    ReadBuff[ReadNum] = 0;
                    std::cout << ReadBuff;
                }
            }
            system("pause");  // use Ctrl+F5 or Debug >> Start Without debugging instead.
            TerminateProcess(pi.hProcess, 1);
        }
    }

    return success;
}