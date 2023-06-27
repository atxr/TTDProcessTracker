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
					printf("- %d\n", buffer[i]);
					// TODO If so, attach TTD and resume it
				}
			}

			::Sleep(200);
		}
	}

	CloseHandle(hDevice);
	return 0;
}
