#include "TTDProcessTracker.h"
#include "ProcessTracker.h"

Globals g_Globals;

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	InitializeListHead(&g_Globals.ItemsHead);
	g_Globals.ItemsCount = 0;
	g_Globals.TrackedPid = 0;
	g_Globals.Mutex.Init();

	DriverObject->DriverUnload = TTDProcessTrackerUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TTDProcessTrackerDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_READ] = TTDProcessTrackerRead;

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

	KdPrint(("PriorityBooster loaded\n"));
	return status;
}

_Use_decl_annotations_
void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject) {
	AutoLock<FastMutex> lock(g_Globals.Mutex);

	while (!IsListEmpty(&g_Globals.ItemsHead)) {
		auto entry = RemoveHeadList(&g_Globals.ItemsHead);
		ExFreePool(CONTAINING_RECORD(entry, FullItem<ULONG>, Entry));
	}

	UNICODE_STRING symName = SYMLINK_NAME;
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
		g_Globals.TrackedPid = pid_data->pid;
		KdPrint(("IOCTL_TTDPROCESSTRACKER_INIT with PID: %d\n", g_Globals.TrackedPid));
		break;
	}

	case IOCTL_TTDPROCESSTRACKER_STOP: {
		g_Globals.TrackedPid = 0;
		KdPrint(("IOCTL_TTDPROCESSTRACKER_STOP\n"));
	}

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

_Use_decl_annotations_
NTSTATUS TTDProcessTrackerRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);

	auto status = STATUS_SUCCESS;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto len = stack->Parameters.Read.Length;
	auto count = 0;
	NT_ASSERT(Irp->MdlAddress);

	auto buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
	if (!buffer) {
		status = STATUS_INSUFFICIENT_RESOURCES;
	}
	else {
		AutoLock<FastMutex> lock(g_Globals.Mutex);
		for (;;) {
			if (IsListEmpty(&g_Globals.ItemsHead))
				break;

			auto entry = RemoveHeadList(&g_Globals.ItemsHead);
			auto item = CONTAINING_RECORD(entry, FullItem<ULONG>, Entry);
			ULONG size = sizeof(ULONG);
			if (len < size) {
				// Not enough room in the user's buffer.
				InsertHeadList(&g_Globals.ItemsHead, entry);
				break;
			}

			g_Globals.ItemsCount--;
			::memcpy(buffer, &item->Data, size);
			len -= size;
			buffer += size;
			count += size;

			// free the item after copy
			ExFreePool(item);
		}
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = count;
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
	if (g_Globals.TrackedPid == 0) {
		KdPrint(("Cannot track process, no PID set\n"));
		return;
	}

	if (Create && HandleToUlong(ParentId) == g_Globals.TrackedPid) {
		KdPrint(("TTDPROCESSTRACKER CreateProcessNotify: Process %d with parent %d created\n", HandleToUlong(ProcessId), HandleToUlong(ParentId)));
		NTSTATUS status;

		PEPROCESS process;
		status = PsLookupProcessByProcessId(ParentId, &process);
		if (!NT_SUCCESS(status)) {
			KdPrint(("IOCTL_TTDPROCESSTRACKER_RESUME failed to open process with PID: %d\n", g_Globals.TrackedPid));
			return;
		}

		typedef NTSTATUS(*PSSUSPENDPROCESS)(PEPROCESS p);
		UNICODE_STRING temp = RTL_CONSTANT_STRING(L"PsSuspendProcess");
		PSSUSPENDPROCESS PsSuspendProcess = (PSSUSPENDPROCESS)MmGetSystemRoutineAddress(&temp);
		status = PsSuspendProcess(process);
		if (!NT_SUCCESS(status)) {
			KdPrint(("TTDPROCESSTRACKER CreateProcessNotify failed to suspend process with PID: %d\n", g_Globals.TrackedPid));
			return;
		}

		auto suspendedInfo = (FullItem<ULONG>*)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(FullItem<ULONG>), 'TTD');
		if (!suspendedInfo) {
			KdPrint(("CreateProcessNotify: Failed to allocate memory for suspendedInfo\n"));
			return;
		}

		suspendedInfo->Data = HandleToUlong(ProcessId);
		status = PushItem(&suspendedInfo->Entry);
		if (!NT_SUCCESS(status)) {
			KdPrint(("TTDPROCESSTRACKER CreateProcessNotify failed to suspend process with PID: %d\n", g_Globals.TrackedPid));
			return;
		}

		KdPrint(("TTDPROCESSTRACKER CreateProcessNotify: Process %d with parent %d suspended\n", HandleToUlong(ProcessId), HandleToUlong(ParentId)));
	}

	return;
}

NTSTATUS PushItem(LIST_ENTRY* Entry) {
	AutoLock<FastMutex> lock(g_Globals.Mutex);
	if (g_Globals.ItemsCount >= MAX_SUSPENDED_PIDS) {
		KdPrint(("TTDPROCESSTRACKER PushItem: Suspended PIDs list is full\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	InsertTailList(&g_Globals.ItemsHead, Entry);
	g_Globals.ItemsCount++;
	return STATUS_SUCCESS;
}
