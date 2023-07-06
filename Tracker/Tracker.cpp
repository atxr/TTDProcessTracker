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
		printf("Stopping tracker...\n");
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

int GetCurrentPath(char* out) {
	char buffer[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, buffer, MAX_PATH);
	char* end = strrchr(buffer, '\\');
	if (!end) {
		return 1;
	}

	*end = 0;
	memcpy(out, buffer, end - buffer + 1);
	return 0;
}

int main(int argc, const char* argv[])
{
	if (argc != 3) {
		printf("Usage: tracker <program to trace> <out directory>\n");
		return 1;
	}

	DWORD current_pid = GetCurrentProcessId();

	hDevice = CreateFile(L"\\\\.\\TTDProcessTracker", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE)
		return Error("Failed to open device");

	DWORD tmp;
	PID_DATA current_pid_data = { current_pid };
	BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_INIT, &current_pid_data, sizeof(current_pid_data), nullptr, 0, &tmp, nullptr);
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

	char path[MAX_PATH];
	if (GetCurrentPath((char*)path) == 1) {
		fprintf(stderr, "Failed to retreive current path\n");
		return 1;
	}
	char ttd_path[MAX_PATH] = { 0 };
	snprintf(ttd_path, MAX_PATH, "%s\\TTD.exe", path);
	const char ttd_launch_format[] = "TTD.exe -out %s -launch %s";
	const char ttd_attach_format[] = "TTD.exe -out %s -attach %d -onInitComplete TtdInitCompleteEvent1480";

	ULONG buffer[64];
	char cmd[MAX_LENGTH];
	SHORT copied;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));

	printf("ttdpath=%s\n", ttd_path);
	copied = snprintf(cmd, MAX_LENGTH, ttd_launch_format, argv[2], argv[1]);
	if (!CreateProcessA(ttd_path, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		return Error("Failed to start TTD process");
	}

	if (SetConsoleCtrlHandler(CtrlHandler, TRUE))
	{
		printf("Press CTRL+C to stop tracking\n");
		int count = 0;
		for (;;) {
			// Check if a child process has been caught by the driver
			DWORD returned;
			if (!ReadFile(hDevice, buffer, sizeof(buffer), &returned, nullptr))
				return Error("Failed to read from device");

			returned /= sizeof(ULONG);

			if (returned != 0) {
				printf("Driver suspended %d child process(es) with PID:\n", returned);

				for (unsigned int i = 0; i < returned; i++) {
					// First time process suspend ie original TTD.exe
					// Stop tracking current_pid to avoid tracking the CreateProcess calls
					if (count == 0) {
						BOOL success = DeviceIoControl(hDevice, IOCTL_TTDPROCESSTRACKER_STOP, &current_pid_data, sizeof(current_pid_data), nullptr, 0, &returned, nullptr);
						printf("- %ul (init)\n", buffer[0]);
						if (!success) {
							return Error("Stop tracker failed");
						}
						count++;
						continue;
					}

					ULONG suspended_pid = buffer[i];
					copied = snprintf(cmd, MAX_LENGTH, ttd_attach_format, argv[2], suspended_pid);

					STARTUPINFOA si1;
					PROCESS_INFORMATION pi1;
					ZeroMemory(&si, sizeof(si1));
					ZeroMemory(&pi1, sizeof(pi1));
					if (!CreateProcessA(ttd_path, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si1, &pi1)) {
						return Error("Failed to start TTD process");
					}

					HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, suspended_pid);
					NtResumeProcess(process);

					count++;
				}
			}

			Sleep(200);
		}
	}

	CloseHandle(hDevice);
	return 0;
}
