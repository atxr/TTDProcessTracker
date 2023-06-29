#include <iostream>
#include <Windows.h>
#include "../TTDProcessTracker/TTDProcessTracker.h"

#define MAX_LENGTH 1024

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
		printf("CtrlHandler\n");
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

int wmain(int argc, const wchar_t* argv[])
{
	if (argc != 3) {
		printf("Usage: tracker <PID to track> <out directory>\n");
		return 1;
	}

	hDevice = CreateFile(L"\\\\.\\TTDProcessTracker", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE)
		return Error("Failed to open device");

	DWORD tmp;
	wchar_t* end;
	PID_DATA data = { wcstoul(argv[1], &end, 0) };
	BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_INIT, &data, sizeof(data), nullptr, 0, &tmp, nullptr);
	if (!success) {
		return Error("Failed to init tracker");
	}

	typedef VOID(*PNTRESUMEPROCESS)(HANDLE p);
	HMODULE ntdll = GetModuleHandle(L"ntdll.dll");
	if (!ntdll) {
		return Error("Failed to load ntdll.dll");
	}
	PNTRESUMEPROCESS NtResumeProcess = (PNTRESUMEPROCESS)GetProcAddress(ntdll, "NtResumeProcess");
	if (!NtResumeProcess) {
		return Error("Failed to get NtResumeProcess address");
	}

	ULONG buffer[64];
	if (SetConsoleCtrlHandler(CtrlHandler, TRUE))
	{
		printf("Press CTRL+C to stop tracking\n");
		int count = 0;
		for (;;) {
			// Check if a child process has been caught by the driver
			DWORD returned;
			if (!::ReadFile(hDevice, buffer, sizeof(buffer), &returned, nullptr))
				return Error("Failed to read from device");

			returned /= sizeof(ULONG);

			if (returned != 0) {
				printf("Driver suspended %d child process(es) with PID:\n", returned);
				for (int i = 0; i < returned; i++) {
					ULONG child_pid = buffer[i];
					if (count == 0) {
						// Stop tracker
						success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_STOP, nullptr, 0, nullptr, 0, &tmp, nullptr);
						if (!success) {
							return Error("Failed to stop tracker");
						}

						// Init tracker again with child PID
						PID_DATA newdata = { child_pid };
						success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_INIT, &newdata, sizeof(newdata), nullptr, 0, &tmp, nullptr);
						if (!success) {
							return Error("Failed to init tracker");
						}

						// Drop the other PIDs if any
						i = returned;
					}

					else {
						const wchar_t path[] = L"C:\\Program Files\\WindowsApps\\Microsoft.WinDbg_1.2306.12001.0_x64__8wekyb3d8bbwe\\amd64\\ttd\\TTD.exe";
						wchar_t cmd[MAX_LENGTH];
						const wchar_t out[] = L"out";
						const wchar_t* arg[2] = { argv[2], argv[1] };
						SHORT copied = _snwprintf_s(cmd, MAX_LENGTH, L"TTD.exe -out %s -attach %s -onInitComplete TtdInitCompleteEvent1480 1>path/to/temp/file 2>&1", argv[2], argv[1]);

						STARTUPINFO si;
						PROCESS_INFORMATION pi;
						CreateProcessW(path, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
					}

					HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, child_pid);
					NtResumeProcess(process);

					printf("- %ul\n", buffer[i]);
					count++;
				}
			}

			::Sleep(200);
		}
	}

	CloseHandle(hDevice);
	return 0;
}
