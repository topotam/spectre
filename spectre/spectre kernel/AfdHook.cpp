/*
 * This file is subject to the terms and conditions defined in
 * file 'LICENSE', which is part of this source code package.
 *
 * COPYRIGHT Bill Demirkapi 2020
 */
#include "AfdHook.h"

/**
	Initialize the AfdHook class, specifically the hooks on all FILE_OBJECTs with the device \Device\Afd.
*/
AfdHook::AfdHook (
	VOID
	)
{
	//
	// Create the FileObjHook instance.
	//
	this->AfdDeviceHook = new (NonPagedPool, AFD_FILE_HOOK_TAG) FileObjHook(AFD_DEVICE_BASE_NAME, DirectHook, AfdHook::HookAfdIoctl, NULL);
	if (this->AfdDeviceHook == NULL)
	{
		DBGPRINT("AfdHook!AfdHook: Failed to allocate memory for Afd FileObjHook.");
		return;
	}
}

/**
	Hook for IOCTL requests made against \Device\Afd.
	@param OriginalFunction - The original IOCTL function for the Afd driver.
	@param DeviceObject - The real device object associated with the Afd driver.
	@param Irp - The IRP for the current request.
*/
NTSTATUS
AfdHook::HookAfdIoctl (
	_In_ DRIVER_DISPATCH OriginalFunction,
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp
	)
{
	NTSTATUS returnStatus;
	PAFD_RECV_INFO recvInformation;
	PIO_STACK_LOCATION irpStackLocation;

	BOOLEAN deviceControlRequest;
	PVOID inputBuffer;
	ULONG inputBufferLength;
	DWORD ioControlCode;

	//
	// TODO: Remove me!!! Just for testing.
	//
	CONST CHAR testbuf[] = "\xef\xbe\xad\xdethis is a test";

	irpStackLocation = IoGetCurrentIrpStackLocation(Irp);
	deviceControlRequest = FALSE;
	inputBuffer = NULL;
	ioControlCode = 0;

	//
	// Before calling the original function we need to save the passed parameters.
	//
	if (irpStackLocation->MajorFunction == IRP_MJ_DEVICE_CONTROL)
	{
		inputBuffer = irpStackLocation->Parameters.DeviceIoControl.Type3InputBuffer;
		inputBufferLength = irpStackLocation->Parameters.DeviceIoControl.InputBufferLength;
		ioControlCode = irpStackLocation->Parameters.DeviceIoControl.IoControlCode;
		deviceControlRequest = TRUE;
	}

	//
	// Grab the actual return value.
	//
	returnStatus = OriginalFunction(DeviceObject, Irp);

	//
	// Only process IRP_MJ_DEVICE_CONTROL requests.
	//
	if (deviceControlRequest)
	{
		if (ioControlCode == IOCTL_AFD_RECV)
		{
			recvInformation = RCAST<PAFD_RECV_INFO>(inputBuffer);
			
			//
			// TODO: Remove this later, not necessary.
			//
			DBGPRINT("AfdHook!HookAfdIoctl: recv ProcessId(%i), BufferCount(%i), AfdFlags(%i), status(0x%X)", PsGetCurrentProcessId(), recvInformation->BufferCount, recvInformation->AfdFlags, returnStatus);

			//
			// Dealing with user-mode memory, need to absolutely wrap in a try/catch.
			//
			__try
			{
				ProbeForRead(recvInformation, sizeof(AFD_RECV_INFO), sizeof(ULONG));

				//
				// If the recv wasn't successful, not worth to check.
				//
				if (NT_SUCCESS(returnStatus) == FALSE)
				{
					goto Exit;
				}

				//
				// Sanity check to make sure we have at least one buffer.
				//
				if (recvInformation->BufferCount == 0)
				{
					goto Exit;
				}

				//
				// Make sure the first buffer can fit the magic value.
				//
				if (recvInformation->BufferArray[0].len < sizeof(DWORD))
				{
					goto Exit;
				}

				//
				// Check for the magic value.
				//
				if (*RCAST<DWORD*>(recvInformation->BufferArray[0].buf) != PACKET_MAGIC)
				{
					goto Exit;
				}

				DBGPRINT("AfdHook!HookAfdIoctl: Found magic in first buffer.");
				returnStatus = AfdHook::SendBuffer(irpStackLocation->FileObject, DeviceObject, CCAST<CHAR*>(testbuf), sizeof(testbuf));
				if (NT_SUCCESS(returnStatus) == FALSE)
				{
					DBGPRINT("AfdHook!HookAfdIoctl: Failed to send buffer with status 0x%X.", returnStatus);
					goto Exit;
				}
				DBGPRINT("AfdHook!HookAfdIoctl: Sent buffer!!!!");


			}
			__except (1)
			{

			}
		}
	}

Exit:
	return returnStatus;
}

/**
	Simulates WSPSend() and sends Buffer to the active SocketFileObject.
	@param SocketFileObject - Pointer to the FILE_OBJECT for the target socket.
	@param OriginalDeviceObject - The original device object for the Afd driver.
	@param Buffer - The buffer to send.
	@param BufferSize - The number of bytes in the buffer.
*/
BOOLEAN
AfdHook::SendBuffer (
	_In_ PFILE_OBJECT SocketFileObject,
	_In_ PDEVICE_OBJECT OriginalDeviceObject,
	_In_ CHAR* Buffer,
	_In_ SIZE_T BufferSize
	)
{
	NTSTATUS status;
	PAFD_SEND_INFO sendInfoUsermode;
	SIZE_T sendInfoSize;
	PAFD_WSABUF sendBuffersUsermode;
	SIZE_T sendBuffersSize;
	PCHAR usermodeBuffer;
	HANDLE socketEventHandle;
	PKEVENT socketEvent;
	IO_STATUS_BLOCK dummyIOSB;
	PIRP sendIrp;
	PIO_STACK_LOCATION sendIrpStack;

	sendInfoUsermode = NULL;
	usermodeBuffer = NULL;
	sendBuffersUsermode = NULL;
	sendInfoSize = sizeof(AFD_SEND_INFO);
	sendBuffersSize = sizeof(AFD_WSABUF);
	socketEventHandle = NULL;
	socketEvent = NULL;

	//
	// Since we're simulating a user-mode function, all buffers we give the Afd driver must be in user-mode memory space.
	//

	//
	// First allocate a buffer for the AFD_SEND_INFO structure.
	//
	status = ZwAllocateVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&sendInfoUsermode), 0, &sendInfoSize, MEM_COMMIT, PAGE_READWRITE);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("AfdHook!SendBuffer: Failed to allocate a user-mode buffer for the AFD_SEND_INFO structure with status 0x%X.", status);
		goto Exit;
	}

	//
	// Next allocate a buffer for the AFD_WSABUF structure.
	//
	status = ZwAllocateVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&sendBuffersUsermode), 0, &sendBuffersSize, MEM_COMMIT, PAGE_READWRITE);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("AfdHook!SendBuffer: Failed to allocate a user-mode buffer for the AFD_WSABUF structure with status 0x%X.", status);
		goto Exit;
	}

	//
	// Finally allocate a buffer for the buffer to send.
	//
	status = ZwAllocateVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&usermodeBuffer), 0, &BufferSize, MEM_COMMIT, PAGE_READWRITE);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("AfdHook!SendBuffer: Failed to allocate a user-mode buffer for the buffer to send with status 0x%X.", status);
		goto Exit;
	}

	//
	// Create the event for the socket send operation.
	//
	status = ZwCreateEvent(&socketEventHandle, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("AfdHook!SendBuffer: Failed to create the socket event with status 0x%X.", status);
		goto Exit;
	}

	//
	// Retrieve the event object.
	//
	status = ObReferenceObjectByHandle(socketEvent, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode, RCAST<PVOID*>(&socketEvent), NULL);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("AfdHook!SendBuffer: Failed to reference the event object with status 0x%X.", status);
		goto Exit;
	}

	//
	// Even though we allocated the memory, it's user-mode memory. We need to be careful.
	//
	__try
	{
		//
		// Copy data from the kernel-mode buffer to the user-mode buffer.
		//
		memcpy(usermodeBuffer, Buffer, BufferSize);

		//
		// Fill out the AFD_WSABUF buffer array.
		//
		sendBuffersUsermode->buf = usermodeBuffer;
		sendBuffersUsermode->len = SCAST<UINT>(BufferSize);

		//
		// Fill out the AFD_SEND_INFO structure.
		//
		memset(sendInfoUsermode, 0, sizeof(AFD_SEND_INFO));
		sendInfoUsermode->BufferArray = sendBuffersUsermode;
		sendInfoUsermode->BufferCount = 1;

		//
		// Allocate the IRP for the send request.
		//
		sendIrp = IoBuildDeviceIoControlRequest(IOCTL_AFD_SEND, OriginalDeviceObject, sendInfoUsermode, sizeof(AFD_SEND_INFO), NULL, 0, FALSE, socketEvent, &dummyIOSB);
		
		//
		// This shouldn't be NULL, sanity check.
		//
		NT_ASSERT(sendIrp);

		//
		// Fill out missing properties in the IRP.
		//
		sendIrp->RequestorMode = UserMode;
		sendIrp->Tail.Overlay.OriginalFileObject = SocketFileObject;

		sendIrpStack = IoGetCurrentIrpStackLocation(sendIrp);
		sendIrpStack->FileObject = SocketFileObject;

		//
		// Sanity checks.
		//
		NT_ASSERT(sendIrpStack->MajorFunction == IRP_MJ_DEVICE_CONTROL);
		NT_ASSERT(sendIrpStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_AFD_SEND);

		//
		// Send the IRP.
		//
		status = IoCallDriver(OriginalDeviceObject, sendIrp);

		//
		// If the send is pending, wait.
		//
		if (status == STATUS_PENDING)
		{
			ZwWaitForSingleObject(socketEventHandle, TRUE, NULL);
			status = dummyIOSB.Status;
		}

		//
		// Did we succeed?
		//
		if (NT_SUCCESS(status) == FALSE)
		{
			DBGPRINT("AfdHook!SendBuffer: Send failed with status 0x%X.", status);
			goto Exit;
		}
	}
	__except (1)
	{
	}
Exit:
	if (sendInfoUsermode)
	{
		sendInfoSize = 0;
		ZwFreeVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&sendInfoUsermode), &sendInfoSize, MEM_RELEASE);
	}
	if (sendBuffersUsermode)
	{
		sendBuffersSize = 0;
		ZwFreeVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&sendBuffersUsermode), &sendBuffersSize, MEM_RELEASE);
	}
	if (usermodeBuffer)
	{
		BufferSize = 0;
		ZwFreeVirtualMemory(NtCurrentProcess(), RCAST<PVOID*>(&usermodeBuffer), &BufferSize, MEM_RELEASE);
	}
	if (socketEvent)
	{
		ObDereferenceObject(socketEvent);
	}
	if (socketEventHandle)
	{
		ZwClose(socketEventHandle);
	}
	return NT_SUCCESS(status);
}