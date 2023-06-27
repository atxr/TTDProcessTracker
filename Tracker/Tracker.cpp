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
		CloseHandle(hDevice);
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

	hDevice = CreateFile(L"\\\\.\\TTDProcessTracker", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE)
		return Error("Failed to open device");

	DWORD returned;
	PID_DATA data = { (unsigned long)atoi(argv[1]) };
	BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_INIT, &data, sizeof(data), nullptr, 0, &returned, nullptr);
	if (!success) {
		return Error("Priority change failed");
	}

	if (SetConsoleCtrlHandler(CtrlHandler, TRUE))
	{
		for (;;) {
			// TODO Check if a child process has been caught by the driver
			// TODO If so, attach TTD and resume it
		}
	}

	CloseHandle(hDevice);
	return 0;
}
