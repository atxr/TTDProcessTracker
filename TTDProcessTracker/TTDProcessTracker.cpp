#include "TTDProcessTracker.h"
#include <ntddk.h>
#include "FastMutex.h"

typedef struct Globals {
	ULONG* TrackedPid;
	FastMutex Mutex;
} Globals;

void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS TTDProcessTrackerCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS TTDProcessTrackerDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);

Globals g_Globals;

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	g_Globals.Mutex.Init();

	DriverObject->DriverUnload = TTDProcessTrackerUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TTDProcessTrackerDeviceControl;

	NTSTATUS status = STATUS_SUCCESS;

	PDEVICE_OBJECT DeviceObject;
	UNICODE_STRING devName = DEVICE_NAME;
	UNICODE_STRING symName = SYMLINK_NAME;

	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create device object (0x%08X)\n", status));
		return status;
	}

	status = IoCreateSymbolicLink(&symName, &devName);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create symlink (0x%08X)\n", status));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	AutoLock<FastMutex> lock(g_Globals.Mutex);
	g_Globals.TrackedPid = (ULONG*)ExAllocatePool2(PagedPool, sizeof(PID_DATA), 'TTD');
	if (!g_Globals.TrackedPid) {
		KdPrint(("Failed to allocate memory for TrackedPid\n"));
		IoDeleteSymbolicLink(&symName);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	*g_Globals.TrackedPid = 0;
	KdPrint(("PriorityBooster loaded\n"));
	return status;
}

_Use_decl_annotations_
void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject) {
	UNICODE_STRING symName = SYMLINK_NAME;
	AutoLock<FastMutex> lock(g_Globals.Mutex);
	ExFreePoolWithTag(g_Globals.TrackedPid, 'TTD');
	IoDeleteDevice(DriverObject->DeviceObject);
	IoDeleteSymbolicLink(&symName);
}

_Use_decl_annotations_
NTSTATUS TTDProcessTrackerCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TTDProcessTrackerDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto status = STATUS_SUCCESS;

	AutoLock<FastMutex> lock(g_Globals.Mutex);
	switch (stack->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_TTDPROCESSTRACKER_INIT:
	{
		if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PID_DATA)) {
			return STATUS_BUFFER_TOO_SMALL;
		}

		PID_DATA* pid_data = (PID_DATA*)stack->Parameters.DeviceIoControl.Type3InputBuffer;
		if (!pid_data) {
			return STATUS_INVALID_PARAMETER;
		}

		// TODO VALID PID TEST HERE

		*g_Globals.TrackedPid = pid_data->pid;
		KdPrint(("IOCTL_TTDPROCESSTRACKER_INIT with PID: %d\n", *g_Globals.TrackedPid));
		break;
	}

	case IOCTL_TTDPROCESSTRACKER_STOP: {
		*g_Globals.TrackedPid = 0;
		KdPrint(("IOCTL_TTDPROCESSTRACKER_STOP\n"));
	}

	case IOCTL_TTDPROCESSTRACKER_RESUME:
	{
		if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PID_DATA)) {
			return STATUS_BUFFER_TOO_SMALL;
		}

		PID_DATA* pid_data = (PID_DATA*)stack->Parameters.DeviceIoControl.Type3InputBuffer;
		if (!pid_data) {
			return STATUS_INVALID_PARAMETER;
		}

		CLIENT_ID client_id = { (HANDLE)pid_data->pid, 0 };
		// TODO VALID PID TEST HERE

		HANDLE handle;
		OBJECT_ATTRIBUTES oa = { sizeof(oa) };
		status = ZwOpenProcess(&handle, PROCESS_ALL_ACCESS, &oa, &client_id);
		if (!NT_SUCCESS(status)) {
			KdPrint(("IOCTL_TTDPROCESSTRACKER_RESUME failed to open process with PID: %d\n", *g_Globals.TrackedPid));
			break;
		}

		// TODO GET THREADS OF PROCESS
		// TODO RESUME THREADS

		//ZwSuspendProcess(handle);

		status = ZwClose(handle);
		if (!NT_SUCCESS(status)) {
			KdPrint(("IOCTL_TTDPROCESSTRACKER_RESUME failed to close process handle with PID: %d\n", *g_Globals.TrackedPid));
			break;
		}
		break;
	}

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

void CreateProcessNotify(
	_In_ HANDLE   ParentId,
	_In_ HANDLE   ProcessId,
	_In_ BOOLEAN  Create
)
{
	AutoLock<FastMutex> lock(g_Globals.Mutex);
	if (*g_Globals.TrackedPid == 0) {
		KdPrint(("Cannot track process, no PID set\n"));
		return;
	}

	if (Create && HandleToUlong(ParentId) == *g_Globals.TrackedPid) {
		KdPrint(("TTDPROCESSTRACKER CreateProcessNotify: Process %d with parent %d created\n", HandleToUlong(ProcessId), HandleToUlong(ParentId)));
		// TODO SUSPEND PROCESS
	}

	return;
}

