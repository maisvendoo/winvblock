/**
 * Copyright (C) 2009-2010, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Bus PnP IRP handling.
 */

#include <ntddk.h>

#include "winvblock.h"
#include "wv_stdlib.h"
#include "portable.h"
#include "irp.h"
#include "driver.h"
#include "device.h"
#include "disk.h"
#include "bus.h"
#include "debug.h"
#include "probe.h"

static NTSTATUS STDCALL
Bus_IoCompletionRoutine (
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp,
  IN PKEVENT Event
 )
{
  KeSetEvent ( Event, 0, FALSE );
  return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS STDCALL bus_pnp__start_dev(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
{
  NTSTATUS status;
  KEVENT event;
  struct bus__type * bus_ptr = bus__get(dev_ptr);
  PDEVICE_OBJECT lower = bus_ptr->LowerDeviceObject;

  if (!lower) {
      *completion_ptr = TRUE;
      return STATUS_SUCCESS;
    }
  KeInitializeEvent ( &event, NotificationEvent, FALSE );
  IoCopyCurrentIrpStackLocationToNext ( Irp );
  IoSetCompletionRoutine ( Irp,
			   ( PIO_COMPLETION_ROUTINE ) Bus_IoCompletionRoutine,
			   ( void * )&event, TRUE, TRUE, TRUE );
  status = IoCallDriver ( lower, Irp );
  if ( status == STATUS_PENDING )
    {
      DBG ( "Locked\n" );
      KeWaitForSingleObject ( &event, Executive, KernelMode, FALSE, NULL );
    }
  if ( NT_SUCCESS ( status = Irp->IoStatus.Status ) )
    {
      dev_ptr->old_state = dev_ptr->state;
      dev_ptr->state = device__state_started;
    }
  status = STATUS_SUCCESS;
  Irp->IoStatus.Status = status;
  IoCompleteRequest ( Irp, IO_NO_INCREMENT );
  *completion_ptr = TRUE;
  return status;
}

NTSTATUS STDCALL bus_pnp__remove_dev(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
{
  NTSTATUS status;
  struct bus__type * bus_ptr;
  struct device__type * walker, * next;
  PDEVICE_OBJECT lower;

  dev_ptr->old_state = dev_ptr->state;
  dev_ptr->state = device__state_deleted;
  Irp->IoStatus.Information = 0;
  Irp->IoStatus.Status = STATUS_SUCCESS;
  IoSkipCurrentIrpStackLocation ( Irp );
  bus_ptr = bus__get(dev_ptr);
  status = IoCallDriver ( bus_ptr->LowerDeviceObject, Irp );
  walker = bus_ptr->first_child;
  while ( walker != NULL )
    {
      next = walker->next_sibling_ptr;
      device__close ( walker );
      IoDeleteDevice ( walker->Self );
      device__free ( walker );
      walker = next;
    }
  bus_ptr->Children = 0;
  bus_ptr->first_child = NULL;
  lower = bus_ptr->LowerDeviceObject;
  if (lower)
    IoDetachDevice(lower);
  IoDeleteDevice ( dev_ptr->Self );
  device__free ( dev_ptr );
  *completion_ptr = TRUE;
  return status;
}

NTSTATUS STDCALL bus_pnp__query_dev_relations(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
{
  NTSTATUS status;
  struct bus__type * bus_ptr;
  winvblock__uint32 count;
  struct device__type * walker;
  PDEVICE_RELATIONS dev_relations;
  PDEVICE_OBJECT lower;

  bus_ptr = bus__get(dev_ptr);
  lower = bus_ptr->LowerDeviceObject;
  if ( Stack->Parameters.QueryDeviceRelations.Type != BusRelations
       || Irp->IoStatus.Information )
    {
      status = Irp->IoStatus.Status;
      if (lower) {
          IoSkipCurrentIrpStackLocation(Irp);
          status = IoCallDriver(lower, Irp);
        }
      *completion_ptr = TRUE;
      return status;
    }
  probe__disks (  );
  count = 0;
  walker = bus_ptr->first_child;
  while ( walker != NULL )
    {
      count++;
      walker = walker->next_sibling_ptr;
    }
  dev_relations = wv_malloc(
      sizeof *dev_relations + (sizeof (PDEVICE_OBJECT) * count)
    );
  if ( dev_relations == NULL )
    {
      Irp->IoStatus.Information = 0;
      status = STATUS_SUCCESS;
      Irp->IoStatus.Status = status;
      if (lower) {
          IoSkipCurrentIrpStackLocation ( Irp );
          status = IoCallDriver(lower, Irp);
        }
      *completion_ptr = TRUE;
      return status;
    }
  dev_relations->Count = count;

  count = 0;
  walker = bus_ptr->first_child;
  while ( walker != NULL )
    {
      ObReferenceObject ( walker->Self );
      dev_relations->Objects[count] = walker->Self;
      count++;
      walker = walker->next_sibling_ptr;
    }
  Irp->IoStatus.Information = ( ULONG_PTR ) dev_relations;
  status = STATUS_SUCCESS;
  Irp->IoStatus.Status = status;
  if (lower) {
      IoSkipCurrentIrpStackLocation ( Irp );
      status = IoCallDriver(lower, Irp);
    }
  *completion_ptr = TRUE;
  return status;
}

NTSTATUS STDCALL bus_pnp__simple(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION Stack,
    IN struct device__type * dev_ptr,
    OUT winvblock__bool_ptr completion_ptr
  )
{
  NTSTATUS status;
  struct bus__type * bus_ptr;
  PDEVICE_OBJECT lower;

  bus_ptr = bus__get(dev_ptr);
  lower = bus_ptr->LowerDeviceObject;
  switch ( Stack->MinorFunction )
    {
      case IRP_MN_QUERY_PNP_DEVICE_STATE:
	Irp->IoStatus.Information = 0;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_QUERY_STOP_DEVICE:
	dev_ptr->old_state = dev_ptr->state;
	dev_ptr->state = device__state_stop_pending;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_CANCEL_STOP_DEVICE:
	dev_ptr->state = dev_ptr->old_state;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_STOP_DEVICE:
	dev_ptr->old_state = dev_ptr->state;
	dev_ptr->state = device__state_stopped;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_QUERY_REMOVE_DEVICE:
	dev_ptr->old_state = dev_ptr->state;
	dev_ptr->state = device__state_remove_pending;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_CANCEL_REMOVE_DEVICE:
	dev_ptr->state = dev_ptr->old_state;
	status = STATUS_SUCCESS;
	break;
      case IRP_MN_SURPRISE_REMOVAL:
	dev_ptr->old_state = dev_ptr->state;
	dev_ptr->state = device__state_surprise_remove_pending;
	status = STATUS_SUCCESS;
	break;
      default:
	status = Irp->IoStatus.Status;
    }

  Irp->IoStatus.Status = status;
  if (lower) {
      IoSkipCurrentIrpStackLocation(Irp);
      status = IoCallDriver(lower, Irp);
    }
  *completion_ptr = TRUE;
  return status;
}
