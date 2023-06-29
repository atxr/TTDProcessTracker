#pragma once
#include <ntifs.h>
#include "FastMutex.h"

#define MAX_SUSPENDED_PIDS 100

template<typename T>
struct FullItem {
	LIST_ENTRY Entry;
	T Data;
};

struct Globals {
	ULONG TrackedPid;
	LIST_ENTRY ItemsHead;
	unsigned int ItemsCount;
	FastMutex Mutex;
};

void TTDProcessTrackerUnload(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS TTDProcessTrackerCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS TTDProcessTrackerDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS TTDProcessTrackerRead(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);

NTSTATUS PushItem(LIST_ENTRY* Entry);
void CreateProcessCallback(
	_Inout_ PEPROCESS Process,
	_In_ HANDLE ProcessId,
	_Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo);

