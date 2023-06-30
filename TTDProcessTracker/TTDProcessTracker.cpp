#include "TTDProcessTracker.h"
#include "ProcessTracker.h"

Globals g_Globals;

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	InitializeListHead(&g_Globals.SuspendedHead);
	InitializeListHead(&g_Globals.TrackedHead);
	g_Globals.SuspendedCount = 0;
	g_Globals.TrackedCount = 0;
	g_Globals.Mutex.Init();

	DriverObject->DriverUnload = TTDProcessTrackerUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = TTDProcessTrackerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TTDProcessTrackerDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_READ] = TTDProcessTrackerRead;

	NTSTATUS status = STATUS_SUCCESS;

	PDEVICE_OBJECT DeviceObject = nullptr;
	UNICODE_STRING devName = DEVICE_NAME;
	UNICODE_STRING symName = SYMLINK_NAME;

	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, TRUE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create device object (0x%08X)\n", status));
		return status;
	}
	DeviceObject->Flags |= DO_DIRECT_IO;

	status = IoCreateSymbolicLink(&symName, &devName);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create symlink (0x%08X)\n", status));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	status = PsSetCreateProcessNotifyRoutineEx(CreateProcessCallback, FALSE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("TTDProcessTracker DriverEntry: Failed to register CreateProcessCallback (0x%08X)\n", status));
	}

	KdPrint(("PriorityBooster loaded\n"));
	return status;
}

_Use_decl_annotations_
void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject) {
	AutoLock<FastMutex> lock(g_Globals.Mutex);
	while (!IsListEmpty(&g_Globals.SuspendedHead)) {
		auto entry = RemoveHeadList(&g_Globals.SuspendedHead);
		ExFreePool(CONTAINING_RECORD(entry, FullItem<ULONG>, Entry));
	}

	while (!IsListEmpty(&g_Globals.TrackedHead)) {
		auto entry = RemoveHeadList(&g_Globals.TrackedHead);
		ExFreePool(CONTAINING_RECORD(entry, FullItem<ULONG>, Entry));
	}

	NTSTATUS status = PsSetCreateProcessNotifyRoutineEx(CreateProcessCallback, TRUE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("TTDProcessTrackerUnload: Failed to unregister CreateProcessCallback (0x%08X)\n", status));
	}


	UNICODE_STRING symName = SYMLINK_NAME;
	IoDeleteDevice(DriverObject->DeviceObject);
	IoDeleteSymbolicLink(&symName);

	KdPrint(("PriorityBooster unloaded\n"));
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

	switch (stack->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_TTDPROCESSTRACKER_INIT:
	{
		// FIXME right size?
		if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(PID_DATA)) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PID_DATA* pid_data = (PID_DATA*)Irp->AssociatedIrp.SystemBuffer;
		if (pid_data == nullptr) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// TODO VALID PID TEST HERE

		// Push the process id to the tracked pid list
		auto trackedEntry = (FullItem<ULONG>*)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(FullItem<ULONG>), 'TTD');
		if (!trackedEntry) {
			KdPrint(("TTDProcessTrackerDeviceControl: Failed to allocate memory for trackedEntry\n"));
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		trackedEntry->Data = pid_data->pid;
		status = PushItem(&g_Globals.TrackedHead, &trackedEntry->Entry);
		if (!NT_SUCCESS(status)) {
			KdPrint(("TTDPROCESSTRACKER CreateProcessCallback failed to suspend process with PID: %d\n", trackedEntry->Data));
			break;
		}

		KdPrint(("IOCTL_TTDPROCESSTRACKER_INIT with PID: %d\n", trackedEntry->Data));
		break;
	}

	case IOCTL_TTDPROCESSTRACKER_STOP: {
		AutoLock<FastMutex> lock(g_Globals.Mutex);

		while (!IsListEmpty(&g_Globals.TrackedHead)) {
			auto entry = RemoveHeadList(&g_Globals.TrackedHead);
			ExFreePool(CONTAINING_RECORD(entry, FullItem<ULONG>, Entry));
		}

		KdPrint(("IOCTL_TTDPROCESSTRACKER_STOP\n"));
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
		for (;;) {
			if (IsListEmpty(&g_Globals.SuspendedHead))
				break;

			AutoLock<FastMutex> lock(g_Globals.Mutex);
			auto entry = RemoveHeadList(&g_Globals.SuspendedHead);
			auto item = CONTAINING_RECORD(entry, FullItem<ULONG>, Entry);
			ULONG size = sizeof(ULONG);
			if (len < size) {
				// Not enough room in the user's buffer.
				InsertHeadList(&g_Globals.SuspendedHead, entry);
				break;
			}

			g_Globals.SuspendedCount--;
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

void CreateProcessCallback(
	_Inout_ PEPROCESS Process,
	_In_ HANDLE ProcessId,
	_Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	// If we are not tracking a process, or if the process exits just return
	if (IsListEmpty(&g_Globals.TrackedHead) || CreateInfo == nullptr) {
		return;
	}

	// If the process pid matches the one we are tracking
	PLIST_ENTRY TrackedEntry = &g_Globals.TrackedHead;
	for (unsigned int i = 0; i < g_Globals.TrackedCount; i++) {
		auto TrackedItem = CONTAINING_RECORD(TrackedEntry, FullItem<ULONG>, Entry);

		if (HandleToUlong(CreateInfo->ParentProcessId) == TrackedItem->Data) {
			KdPrint(("TTDPROCESSTRACKER CreateProcessCallback: Process %d with parent %d created\n", HandleToUlong(ProcessId), HandleToUlong(CreateInfo->ParentProcessId)));
			NTSTATUS status;

			// Suspend the process
			typedef NTSTATUS(*PSSUSPENDPROCESS)(PEPROCESS p);
			UNICODE_STRING temp = RTL_CONSTANT_STRING(L"PsSuspendProcess");
			PSSUSPENDPROCESS PsSuspendProcess = (PSSUSPENDPROCESS)MmGetSystemRoutineAddress(&temp);
			status = PsSuspendProcess(Process);
			if (!NT_SUCCESS(status)) {
				KdPrint(("TTDPROCESSTRACKER CreateProcessCallback failed to suspend process with PID: %d\n", TrackedItem->Data));
				return;
			}

			// Push the process id to the notification list
			auto suspendedInfo = (FullItem<ULONG>*)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(FullItem<ULONG>), 'TTD');
			if (!suspendedInfo) {
				KdPrint(("CreateProcessCallback: Failed to allocate memory for suspendedInfo\n"));
				return;
			}

			suspendedInfo->Data = HandleToUlong(ProcessId);
			status = PushItem(&g_Globals.SuspendedHead, &suspendedInfo->Entry);
			if (!NT_SUCCESS(status)) {
				KdPrint(("TTDPROCESSTRACKER CreateProcessCallback failed to suspend process with PID: %d\n", TrackedItem->Data));
				return;
			}

			KdPrint(("TTDPROCESSTRACKER CreateProcessCallback: Process %d with parent %d suspended\n", HandleToUlong(ProcessId), HandleToUlong(CreateInfo->ParentProcessId)));
			break;
		}

		else {
			TrackedEntry = TrackedEntry->Flink;
		}
	}

	return;
}

NTSTATUS PushItem(PLIST_ENTRY ListHead, PLIST_ENTRY Entry) {
	if (g_Globals.SuspendedCount >= MAX_SUSPENDED_PIDS) {
		KdPrint(("TTDPROCESSTRACKER PushItem: Suspended PIDs list is full\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	AutoLock<FastMutex> lock(g_Globals.Mutex);
	InsertTailList(ListHead, Entry);
	g_Globals.SuspendedCount++;
	return STATUS_SUCCESS;
}
