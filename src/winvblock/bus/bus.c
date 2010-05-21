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
 * Bus specifics
 *
 */

#include <ntddk.h>

#include "winvblock.h"
#include "portable.h"
#include "irp.h"
#include "driver.h"
#include "device.h"
#include "bus.h"
#include "bus_pnp.h"
#include "bus_dev_ctl.h"
#include "debug.h"

static PDEVICE_OBJECT boot_bus_fdo = NULL;
static LIST_ENTRY bus_list;
static KSPIN_LOCK bus_list_lock;
/* Forward declarations */
static device__free_decl (
  free_bus
 );
static device__create_pdo_decl (
  create_pdo
 );

/**
 * Tear down the global, bus-common environment
 */
void
bus__finalize (
  void
 )
{
  UNICODE_STRING DosDeviceName;

  DBG ( "Entry\n" );
  RtlInitUnicodeString ( &DosDeviceName,
			 L"\\DosDevices\\" winvblock__literal_w );
  IoDeleteSymbolicLink ( &DosDeviceName );
  boot_bus_fdo = NULL;
  DBG ( "Exit\n" );
}

/**
 * Add a child node to the bus
 *
 * @v bus_ptr           Points to the bus receiving the child
 * @v dev_ptr           Points to the child device to add
 *
 * Returns TRUE for success, FALSE for failure
 */
winvblock__lib_func winvblock__bool STDCALL
bus__add_child (
  IN OUT bus__type_ptr bus_ptr,
  IN device__type_ptr dev_ptr
 )
{
  /**
   * @v dev_obj_ptr         The new node's device object
   * @v bus_ptr             A pointer to the bus device's details
   * @v walker              Walks the child nodes
   */
  PDEVICE_OBJECT dev_obj_ptr;
  device__type_ptr walker;

  DBG ( "Entry\n" );
  if ( ( bus_ptr == NULL ) || ( dev_ptr == NULL ) )
    {
      DBG ( "No bus or no device!\n" );
      return FALSE;
    }
  /*
   * Create the child device
   */
  dev_obj_ptr = device__create_pdo ( dev_ptr );
  if ( dev_obj_ptr == NULL )
    {
      DBG ( "PDO creation failed!\n" );
      return FALSE;
    }

  /*
   * Re-purpose dev_ptr to point into the PDO's device
   * extension space.  We don't need the original details anymore
   */
  dev_ptr = dev_obj_ptr->DeviceExtension;
  dev_ptr->Parent = bus_ptr->device->Self;
  dev_ptr->next_sibling_ptr = NULL;
  /*
   * Initialize the device.  For disks, this routine is responsible for
   * determining the disk's geometry appropriately for AoE/RAM/file disks
   */
  dev_ptr->ops.init ( dev_ptr );
  dev_obj_ptr->Flags &= ~DO_DEVICE_INITIALIZING;
  /*
   * Add the new device's extension to the bus' list of children
   */
  if ( bus_ptr->first_child_ptr == NULL )
    {
      bus_ptr->first_child_ptr = ( winvblock__uint8_ptr ) dev_ptr;
    }
  else
    {
      walker = ( device__type_ptr ) bus_ptr->first_child_ptr;
      while ( walker->next_sibling_ptr != NULL )
	walker = walker->next_sibling_ptr;
      walker->next_sibling_ptr = dev_ptr;
    }
  bus_ptr->Children++;
  if ( bus_ptr->PhysicalDeviceObject != NULL )
    {
      IoInvalidateDeviceRelations ( bus_ptr->PhysicalDeviceObject,
				    BusRelations );
    }
  DBG ( "Exit\n" );
  return TRUE;
}

static
irp__handler_decl (
  sys_ctl
 )
{
  bus__type_ptr bus_ptr = bus__get_ptr ( DeviceExtension );
  DBG ( "...\n" );
  IoSkipCurrentIrpStackLocation ( Irp );
  *completion_ptr = TRUE;
  return IoCallDriver ( bus_ptr->LowerDeviceObject, Irp );
}

static
irp__handler_decl (
  power
 )
{
  bus__type_ptr bus_ptr = bus__get_ptr ( DeviceExtension );
  PoStartNextPowerIrp ( Irp );
  IoSkipCurrentIrpStackLocation ( Irp );
  *completion_ptr = TRUE;
  return PoCallDriver ( bus_ptr->LowerDeviceObject, Irp );
}

static irp__handling handling_table[] = {
  /*
   * Major, minor, any major?, any minor?, handler
   */
  {IRP_MJ_SYSTEM_CONTROL, 0, FALSE, TRUE, sys_ctl}
  ,
  {IRP_MJ_POWER, 0, FALSE, TRUE, power}
  ,
  {IRP_MJ_DEVICE_CONTROL, 0, FALSE, TRUE, bus_dev_ctl__dispatch}
  ,
  {IRP_MJ_PNP, 0, FALSE, TRUE, bus_pnp__simple}
  ,
  {IRP_MJ_PNP, IRP_MN_START_DEVICE, FALSE, FALSE, bus_pnp__start_dev}
  ,
  {IRP_MJ_PNP, IRP_MN_REMOVE_DEVICE, FALSE, FALSE, bus_pnp__remove_dev}
  ,
  {IRP_MJ_PNP, IRP_MN_QUERY_DEVICE_RELATIONS, FALSE, FALSE,
   bus_pnp__query_dev_relations}
};

NTSTATUS STDCALL
Bus_GetDeviceCapabilities (
  IN PDEVICE_OBJECT DeviceObject,
  IN PDEVICE_CAPABILITIES DeviceCapabilities
 )
{
  IO_STATUS_BLOCK ioStatus;
  KEVENT pnpEvent;
  NTSTATUS status;
  PDEVICE_OBJECT targetObject;
  PIO_STACK_LOCATION irpStack;
  PIRP pnpIrp;

  RtlZeroMemory ( DeviceCapabilities, sizeof ( DEVICE_CAPABILITIES ) );
  DeviceCapabilities->Size = sizeof ( DEVICE_CAPABILITIES );
  DeviceCapabilities->Version = 1;
  DeviceCapabilities->Address = -1;
  DeviceCapabilities->UINumber = -1;

  KeInitializeEvent ( &pnpEvent, NotificationEvent, FALSE );
  targetObject = IoGetAttachedDeviceReference ( DeviceObject );
  pnpIrp =
    IoBuildSynchronousFsdRequest ( IRP_MJ_PNP, targetObject, NULL, 0, NULL,
				   &pnpEvent, &ioStatus );
  if ( pnpIrp == NULL )
    {
      status = STATUS_INSUFFICIENT_RESOURCES;
    }
  else
    {
      pnpIrp->IoStatus.Status = STATUS_NOT_SUPPORTED;
      irpStack = IoGetNextIrpStackLocation ( pnpIrp );
      RtlZeroMemory ( irpStack, sizeof ( IO_STACK_LOCATION ) );
      irpStack->MajorFunction = IRP_MJ_PNP;
      irpStack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
      irpStack->Parameters.DeviceCapabilities.Capabilities =
	DeviceCapabilities;
      status = IoCallDriver ( targetObject, pnpIrp );
      if ( status == STATUS_PENDING )
	{
	  KeWaitForSingleObject ( &pnpEvent, Executive, KernelMode, FALSE,
				  NULL );
	  status = ioStatus.Status;
	}
    }
  ObDereferenceObject ( targetObject );
  return status;
}

static NTSTATUS STDCALL
attach_fdo (
  IN PDRIVER_OBJECT DriverObject,
  IN PDEVICE_OBJECT PhysicalDeviceObject
 )
{
  PLIST_ENTRY walker;
  bus__type_ptr bus_ptr;
  NTSTATUS status;
  PUNICODE_STRING dev_name = NULL;
  PDEVICE_OBJECT fdo = NULL;
  device__type_ptr dev_ptr;

  DBG ( "Entry\n" );
  /*
   * Search for the associated bus
   */
  walker = bus_list.Flink;
  while ( walker != &bus_list )
    {
      bus_ptr = CONTAINING_RECORD ( walker, bus__type, tracking );
      if ( bus_ptr->PhysicalDeviceObject == PhysicalDeviceObject )
	break;
      walker = walker->Flink;
    }
  /*
   * If we get back to the list head, we need to create a bus
   */
  if ( walker == &bus_list )
    {
      DBG ( "No bus->PDO association.  Creating a new bus\n" );
      bus_ptr = bus__create (  );
      if ( bus_ptr == NULL )
	return Error ( "Could not create a bus!\n",
		       STATUS_INSUFFICIENT_RESOURCES );
    }
  /*
   * This bus might have an associated name
   */
  if ( bus_ptr->named )
    dev_name = &bus_ptr->dev_name;

  status =
    IoCreateDevice ( DriverObject, sizeof ( device__type_ptr ), dev_name,
		     FILE_DEVICE_CONTROLLER, FILE_DEVICE_SECURE_OPEN, FALSE,
		     &fdo );
  if ( !NT_SUCCESS ( status ) )
    {
      return Error ( "IoCreateDevice", status );
    }
  /*
   * DosDevice symlink
   */
  if ( bus_ptr->named )
    status =
      IoCreateSymbolicLink ( &bus_ptr->dos_dev_name, &bus_ptr->dev_name );
  if ( !NT_SUCCESS ( status ) )
    {
      IoDeleteDevice ( fdo );
      return Error ( "IoCreateSymbolicLink", status );
    }

  /*
   * Set associations for the bus, device, FDO, PDO
   */
  dev_ptr = bus_ptr->device;
  *( ( device__type_ptr * ) fdo->DeviceExtension ) = dev_ptr;	/* Careful */
  dev_ptr->DriverObject = DriverObject;
  dev_ptr->Self = fdo;

  bus_ptr->PhysicalDeviceObject = PhysicalDeviceObject;
  fdo->Flags |= DO_DIRECT_IO;	/* FIXME? */
  fdo->Flags |= DO_POWER_INRUSH;	/* FIXME? */
  /*
   * Add the bus to the device tree
   */
  if ( PhysicalDeviceObject != NULL )
    {
      bus_ptr->LowerDeviceObject =
	IoAttachDeviceToDeviceStack ( fdo, PhysicalDeviceObject );
      if ( bus_ptr->LowerDeviceObject == NULL )
	{
	  IoDeleteDevice ( fdo );
	  return Error ( "IoAttachDeviceToDeviceStack",
			 STATUS_NO_SUCH_DEVICE );
	}
    }
  fdo->Flags &= ~DO_DEVICE_INITIALIZING;
#ifdef RIS
  dev_ptr->State = Started;
#endif
  DBG ( "Exit\n" );
  return STATUS_SUCCESS;
}

/**
 * Create a new bus
 *
 * @ret bus_ptr         The address of a new bus, or NULL for failure
 *
 * See the header file for additional details
 */
winvblock__lib_func bus__type_ptr
bus__create (
  void
 )
{
  device__type_ptr dev_ptr;
  bus__type_ptr bus_ptr;

  /*
   * Try to create a device
   */
  dev_ptr = device__create (  );
  if ( dev_ptr == NULL )
    goto err_nodev;
  /*
   * Bus devices might be used for booting and should
   * not be allocated from a paged memory pool
   */
  bus_ptr = ExAllocatePool ( NonPagedPool, sizeof ( bus__type ) );
  if ( bus_ptr == NULL )
    goto err_nobus;
  RtlZeroMemory ( bus_ptr, sizeof ( bus__type ) );
  /*
   * Track the new device in our global list
   */
  ExInterlockedInsertTailList ( &bus_list, &bus_ptr->tracking,
				&bus_list_lock );
  /*
   * Populate non-zero device defaults
   */
  bus_ptr->device = dev_ptr;
  bus_ptr->prev_free = dev_ptr->ops.free;
  dev_ptr->TODO_temp_measure = dev_ptr;
  dev_ptr->ops.create_pdo = create_pdo;
  dev_ptr->ops.free = free_bus;
  dev_ptr->ext = bus_ptr;
  dev_ptr->IsBus = TRUE;
  dev_ptr->State = NotStarted;
  dev_ptr->OldState = NotStarted;
  bus_ptr->Children = 0;
  bus_ptr->first_child_ptr = NULL;
  KeInitializeSpinLock ( &bus_ptr->SpinLock );
  /*
   * Register the default bus IRP handling table
   */
  irp__reg_table ( &dev_ptr->irp_handler_chain, handling_table );

  return bus_ptr;

err_nobus:

  device__free ( dev_ptr );
err_nodev:

  return NULL;
}

/**
 * Create a bus PDO
 *
 * @v dev_ptr           Populate PDO dev. ext. space from these details
 * @ret pdo_ptr         Points to the new PDO, or is NULL upon failure
 *
 * Returns a Physical Device Object pointer on success, NULL for failure.
 */
static
device__create_pdo_decl (
  create_pdo
 )
{
  PDEVICE_OBJECT pdo_ptr = NULL;
  bus__type_ptr bus_ptr;
  NTSTATUS status;

  /*
   * Note the bus device needing a PDO
   */
  if ( dev_ptr == NULL )
    {
      DBG ( "No device passed\n" );
      return NULL;
    }
  bus_ptr = ( bus__type_ptr ) dev_ptr->ext;
  /*
   * Always create the root-enumerated bus device 
   */
  IoReportDetectedDevice ( driver__obj_ptr, InterfaceTypeUndefined, -1, -1,
			   NULL, NULL, FALSE, &pdo_ptr );
  if ( pdo_ptr == NULL )
    {
      DBG ( "IoReportDetectedDevice() went wrong!\n" );
      return NULL;
    }
  /*
   * We have a PDO.  Note it in the bus
   */
  bus_ptr->PhysicalDeviceObject = pdo_ptr;
  /*
   * Attach FDO to PDO. *sigh*  Note that we do not own the PDO,
   * so we must associate the bus structure with the FDO, instead.
   * Consider that the AddDevice()/attach_fdo() routine takes two parameters,
   * neither of which are guaranteed to be owned by a caller in this driver.
   * Since attach_fdo() associates a bus device, it is forced to walk our
   * global list of bus devices.  Otherwise, it would be easy to pass it here
   */
  status = attach_fdo ( driver__obj_ptr, pdo_ptr );
  if ( !NT_SUCCESS ( status ) )
    {
      DBG ( "attach_fdo() went wrong!\n" );
      goto err_add_dev;
    }
  return pdo_ptr;

err_add_dev:

  IoDeleteDevice ( pdo_ptr );
  return NULL;
}

/**
 * Initialize the global, bus-common environment
 *
 * @ret ntstatus        STATUS_SUCCESS or the NTSTATUS for a failure
 */
extern STDCALL NTSTATUS
bus__init (
  void
 )
{
  bus__type_ptr boot_bus_ptr;

  /*
   * Initialize the global list of devices
   */
  InitializeListHead ( &bus_list );
  KeInitializeSpinLock ( &bus_list_lock );
  /*
   * We handle AddDevice call-backs for the driver
   */
  driver__obj_ptr->DriverExtension->AddDevice = attach_fdo;
  /*
   * Establish the boot bus (required for booting from a WinVBlock disk)
   */
  boot_bus_ptr = bus__create (  );
  if ( boot_bus_ptr == NULL )
    return STATUS_UNSUCCESSFUL;
  RtlInitUnicodeString ( &boot_bus_ptr->dev_name,
			 L"\\Device\\" winvblock__literal_w );
  RtlInitUnicodeString ( &boot_bus_ptr->dos_dev_name,
			 L"\\DosDevices\\" winvblock__literal_w );
  /*
   * Create the PDO, which also attaches the FDO *sigh*
   */
  if ( create_pdo ( boot_bus_ptr->device ) == NULL )
    {
      free_bus ( boot_bus_ptr->device );
      return STATUS_UNSUCCESSFUL;
    }
  boot_bus_fdo = boot_bus_ptr->device->Self;

  return STATUS_SUCCESS;
}

/**
 * Default bus deletion operation
 *
 * @v dev_ptr           Points to the bus device to delete
 */
static
device__free_decl (
  free_bus
 )
{
  bus__type_ptr bus_ptr = ( bus__type_ptr ) dev_ptr->ext;
  /*
   * Un-register the default driver IRP handling table
   */
  irp__unreg_table ( &dev_ptr->irp_handler_chain, handling_table );
  /*
   * Free the "inherited class"
   */
  bus_ptr->prev_free ( dev_ptr );
  /*
   * Track the bus deletion in our global list.  Unfortunately,
   * for now we have faith that a bus won't be deleted twice and
   * result in a race condition.  Something to keep in mind...
   */
  ExInterlockedRemoveHeadList ( bus_ptr->tracking.Blink, &bus_list_lock );

  ExFreePool ( bus_ptr );
}

/**
 * Get a pointer to the boot bus device
 *
 * @ret         A pointer to the boot bus, or NULL
 */
winvblock__lib_func bus__type_ptr
bus__boot (
  void
 )
{
  if ( !boot_bus_fdo )
    {
      DBG ( "No boot bus device!\n" );
      return NULL;
    }
  return bus__get_ptr ( boot_bus_fdo->DeviceExtension );
}