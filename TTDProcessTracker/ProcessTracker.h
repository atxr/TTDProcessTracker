#pragma once
#include <ntifs.h>
#include "FastMutex.h"

template<typename T>
struct FullItem {
	LIST_ENTRY Entry;
	T Data;
};

struct Globals {
	LIST_ENTRY TrackedHead;
	LIST_ENTRY SuspendedHead;
	FastMutex Mutex;
};

void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS TTDProcessTrackerCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS TTDProcessTrackerDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS TTDProcessTrackerRead(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);

void CreateProcessCallback(
	_Inout_ PEPROCESS Process,
	_In_ HANDLE ProcessId,
	_Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo);

