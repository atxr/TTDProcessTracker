#include <iostream>
#include <Windows.h>
#include "../TTDProcessTracker/TTDProcessTracker.h"

HANDLE hDevice;

int Error(const char* message) {
	printf("%s (error=%d)\n", message, GetLastError());
	return 1;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
	switch (fdwCtrlType)
	{
		// Handle the CTRL-C signal.
	case CTRL_C_EVENT:
	{
		DWORD returned;
		BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_STOP, nullptr, 0, nullptr, 0, &returned, nullptr);
		if (!success) {
			return Error("Stop tracker failed");
		}
		return TRUE;
	}

	default:
		return FALSE;
	}
}

int main(int argc, const char* argv[])
{
	if (argc != 2) {
		printf("Usage: tracker <PID to track>\n");
		return 1;
	}

	hDevice = CreateFile(L"\\\\.\\TTDProcessTracker", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE)
		return Error("Failed to open device");

	DWORD returned;
	PID_DATA data = { (unsigned long)atoi(argv[1]) };
	BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_INIT, &data, sizeof(data), nullptr, 0, &returned, nullptr);
	if (!success) {
		return Error("Failed to init tracker");
	}

	ULONG buffer[64];
	if (SetConsoleCtrlHandler(CtrlHandler, TRUE))
	{
		printf("Press CTRL+C to stop tracking\n");
		for (;;) {
			// Check if a child process has been caught by the driver
			DWORD nb;
			if (!::ReadFile(hDevice, buffer, sizeof(buffer), &returned, nullptr))
				return Error("Failed to read from device");

			if (returned != 0) {
				printf("Driver suspended %d child process(es) with PID:\n", returned);
				for (int i = 0; i < returned; i++) {
					/*
							const wchar_t path[] = L"C:\\Program Files\\WindowsApps\\Microsoft.WinDbg_1.2306.12001.0_x64__8wekyb3d8bbwe\\amd64\\ttd\\TTD.exe";
							wchar_t cmd[MAX_LENGTH];
							const wchar_t out[] = L"out";
							const wchar_t* arg[2] = { argv[2], argv[1] };
							SHORT copied = _snwprintf(cmd, MAX_LENGTH, L"TTD.exe -out %s -attach %s -onInitComplete TtdInitCompleteEvent1480 1>path/to/temp/file 2>&1", argv[2], argv[1]);

							STARTUPINFO si;
							PROCESS_INFORMATION pi;
							CreateProcessW(path, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
					*/
					printf("- %d\n", buffer[i]);
				}
			}

			::Sleep(200);
		}
	}

	CloseHandle(hDevice);
	return 0;
}
