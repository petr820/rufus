/*
 * Rufus: The Reliable USB Formatting Utility
 * USB device listing
 * Copyright � 2014 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <commctrl.h>
#include <setupapi.h>

#include "msapi_utf8.h"
#include "rufus.h"
#include "drive.h"
#include "resource.h"
#include "localization.h"
#include "usb.h"

extern StrArray DriveID, DriveLabel;
extern BOOL enable_HDDs, use_fake_units;

/*
 * Get the VID, PID and current device speed
 */
static void GetUSBProperties(char* parent_path, char* device_id, usb_device_props* props)
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	DWORD size;
	DEVINST device_inst;
	USB_NODE_CONNECTION_INFORMATION_EX conn_info;
	USB_NODE_CONNECTION_INFORMATION_EX_V2 conn_info_v2;
	PF_INIT(CM_Get_DevNode_Registry_PropertyA, Cfgmgr32);

	if ((parent_path == NULL) || (device_id == NULL) || (props == NULL)) {
		return;
	}

	props->port = 0;
	size = sizeof(props->port);
	if ( (pfCM_Get_DevNode_Registry_PropertyA != NULL) && 
		 (CM_Locate_DevNodeA(&device_inst, device_id, 0) == CR_SUCCESS) ) {
		pfCM_Get_DevNode_Registry_PropertyA(device_inst, CM_DRP_ADDRESS, NULL, (PVOID)&props->port, &size, 0);
	}

	handle = CreateFileA(parent_path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("could not open hub %s: %s", parent_path, WindowsErrorString());
		goto out;
	}
	memset(&conn_info, 0, sizeof(conn_info));
	size = sizeof(conn_info);
	conn_info.ConnectionIndex = (ULONG)props->port;
	// coverity[tainted_data_argument]
	if (!DeviceIoControl(handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, &conn_info, size, &conn_info, size, &size, NULL)) {
		uprintf("could not get node connection information for device '%s': %s", device_id, WindowsErrorString());
		goto out;
	}

	props->vid = conn_info.DeviceDescriptor.idVendor;
	props->pid = conn_info.DeviceDescriptor.idProduct;
	props->speed = conn_info.Speed + 1;

	// In their great wisdom, Microsoft decided to BREAK the USB speed report between Windows 7 and Windows 8
	if (nWindowsVersion >= WINDOWS_8) {
		memset(&conn_info_v2, 0, sizeof(conn_info_v2));
		size = sizeof(conn_info_v2);
		conn_info_v2.ConnectionIndex = (ULONG)props->port;
		conn_info_v2.Length = size;
		conn_info_v2.SupportedUsbProtocols.Usb300 = 1;
		if (!DeviceIoControl(handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &conn_info_v2, size, &conn_info_v2, size, &size, NULL)) {
			uprintf("could not get node connection information (V2) for device '%s': %s", device_id, WindowsErrorString());
		} else if (conn_info_v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) {
			props->speed = USB_SPEED_SUPER_OR_LATER;
		} else if (conn_info_v2.Flags.DeviceIsSuperSpeedCapableOrHigher) {
			props->is_LowerSpeed = TRUE;
		}
	}

out:
	safe_closehandle(handle);
}

/*
 * Refresh the list of USB devices
 */
BOOL GetUSBDevices(DWORD devnum)
{
	// The first two are standard Microsoft drivers (including the Windows 8 UASP one).
	// The rest are the vendor UASP drivers I know of so far - list may be incomplete!
	const char* storage_name[] = { "USBSTOR", "UASPSTOR", "VUSBSTOR", "ETRONSTOR" };
	const char* scsi_name = "SCSI";
	const char* vhd_name = "Virtual Disk";
	const char* usb_speed_name[USB_SPEED_MAX] = { "USB", "USB 1.0", "USB 1.1", "USB 2.0", "USB 3.0" };
	// Hash table and String Array used to match a Device ID with the parent hub's Device Interface Path
	htab_table htab_devid = HTAB_EMPTY;
	StrArray dev_if_path;
	char letter_name[] = " (?:)";
	BOOL r = FALSE, found = FALSE, is_SCSI;
	HDEVINFO dev_info = NULL;
	SP_DEVINFO_DATA dev_info_data;
	SP_DEVICE_INTERFACE_DATA devint_data;
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A devint_detail_data;
	DEVINST parent_inst, device_inst;
	DWORD size, i, j, k, datatype, drive_index;
	ULONG list_size[ARRAYSIZE(storage_name)], full_list_size;
	HANDLE hDrive;
	LONG maxwidth = 0;
	int s, score, drive_number;
	char drive_letters[27], *device_id, *devid_list = NULL, entry_msg[128];
	char *label, *entry, buffer[MAX_PATH], str[128];
	usb_device_props props;

	IGNORE_RETVAL(ComboBox_ResetContent(hDeviceList));
	StrArrayClear(&DriveID);
	StrArrayClear(&DriveLabel);
	StrArrayCreate(&dev_if_path, 128);

	device_id = (char*)malloc(MAX_PATH);
	if (device_id == NULL)
		goto out;

	// Build a hash table associating a CM Device ID of an USB device with the SetupDI Device Interface Path
	// of its parent hub - this is needed to retrieve the device speed
	dev_info = SetupDiGetClassDevsA(&_GUID_DEVINTERFACE_USB_HUB, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
	if (dev_info != INVALID_HANDLE_VALUE) {
		if (htab_create(DEVID_HTAB_SIZE, &htab_devid)) {
			dev_info_data.cbSize = sizeof(dev_info_data);
			for (i=0; SetupDiEnumDeviceInfo(dev_info, i, &dev_info_data); i++) {

				devint_detail_data = NULL;
				devint_data.cbSize = sizeof(devint_data);
				// Only care about the first interface (MemberIndex 0)
				if ( (SetupDiEnumDeviceInterfaces(dev_info, &dev_info_data, &_GUID_DEVINTERFACE_USB_HUB, 0, &devint_data))
				  && (!SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, NULL, 0, &size, NULL)) 
				  && (GetLastError() == ERROR_INSUFFICIENT_BUFFER) 
				  && ((devint_detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)calloc(1, size)) != NULL) ) {
					devint_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
					if (SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, devint_detail_data, size, &size, NULL)) {

						// Find the Device IDs for all the children of this hub
						if (CM_Get_Child(&device_inst, dev_info_data.DevInst, 0) == CR_SUCCESS) {
							device_id[0] = 0;
							s = StrArrayAdd(&dev_if_path, devint_detail_data->DevicePath);
							if ((s>= 0) && (CM_Get_Device_IDA(device_inst, device_id, MAX_PATH, 0) == CR_SUCCESS)) {
								if ((k = htab_hash(device_id, &htab_devid)) != 0) {
									htab_devid.table[k].data = (void*)(uintptr_t)s;
								}
								while (CM_Get_Sibling(&device_inst, device_inst, 0) == CR_SUCCESS) {
									device_id[0] = 0;
									if (CM_Get_Device_IDA(device_inst, device_id, MAX_PATH, 0) == CR_SUCCESS) {
										if ((k = htab_hash(device_id, &htab_devid)) != 0) {
											htab_devid.table[k].data = (void*)(uintptr_t)s;
										}
									}
								}
							}
						}

					}
					free(devint_detail_data);
				}
			}
		}
		SetupDiDestroyDeviceInfoList(dev_info);
	}
	free(device_id);

	// Build a single list of Device IDs from all the storage enumerators we know of
	full_list_size = 0;
	for (s=0; s<ARRAYSIZE(storage_name); s++) {
		// Get a list of device IDs for all USB storage devices
		// This will be used to find if a device is UASP
		CM_Get_Device_ID_List_SizeA(&list_size[s], storage_name[s],
			CM_GETIDLIST_FILTER_SERVICE|CM_GETIDLIST_FILTER_PRESENT);
		if (list_size[s] != 0)
			full_list_size += list_size[s]-1;	// remove extra NUL terminator
	}
	devid_list = NULL;
	if (full_list_size != 0) {
		full_list_size += 1;	// add extra NUL terminator
		devid_list = (char*)malloc(full_list_size);
		if (devid_list == NULL) {
			uprintf("Could not allocate Device ID list\n");
			return FALSE;
		}
		for (s=0, i=0; s<ARRAYSIZE(storage_name); s++) {
			if (list_size[s] > 1) {
				CM_Get_Device_ID_ListA(storage_name[s], &devid_list[i], list_size[s],
					CM_GETIDLIST_FILTER_SERVICE|CM_GETIDLIST_FILTER_PRESENT);	// 
				// The list_size is sometimes larger than required thus we need to find the real end
				for (i += list_size[s]; i > 2; i--) {
					if ((devid_list[i-2] != '\0') && (devid_list[i-1] == '\0') && (devid_list[i] == '\0'))
						break;
				}
			}
		}
	}

	// Now use SetupDi to enumerate all our storage devices
	dev_info = SetupDiGetClassDevsA(&_GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
	if (dev_info == INVALID_HANDLE_VALUE) {
		uprintf("SetupDiGetClassDevs (Interface) failed: %s\n", WindowsErrorString());
		goto out;
	}
	dev_info_data.cbSize = sizeof(dev_info_data);
	for (i=0; SetupDiEnumDeviceInfo(dev_info, i, &dev_info_data); i++) {
		memset(buffer, 0, sizeof(buffer));
		if (!SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_info_data, SPDRP_ENUMERATOR_NAME,
				&datatype, (LPBYTE)buffer, sizeof(buffer), &size)) {
			uprintf("SetupDiGetDeviceRegistryProperty (Enumerator Name) failed: %s\n", WindowsErrorString());
			continue;
		}
		// UASP drives are listed under SCSI (along with regular SYSTEM drives => "DANGER, WILL ROBINSON!!!")
		is_SCSI = (safe_stricmp(buffer, scsi_name) == 0);
		if ((safe_stricmp(buffer, storage_name[0]) != 0) && (!is_SCSI))
			continue;
		memset(buffer, 0, sizeof(buffer));
		memset(&props, 0, sizeof(props));
		if (!SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_info_data, SPDRP_FRIENDLYNAME,
				&datatype, (LPBYTE)buffer, sizeof(buffer), &size)) {
			uprintf("SetupDiGetDeviceRegistryProperty (Friendly Name) failed: %s\n", WindowsErrorString());
			// We can afford a failure on this call - just replace the name with "USB Storage Device (Generic)"
			safe_strcpy(buffer, sizeof(buffer), lmprintf(MSG_045));
		} else if (safe_strstr(buffer, vhd_name) != NULL) {
			props.is_VHD = TRUE;
		} else if (devid_list != NULL) {
			// Get the properties of the device. We could avoid doing this lookup every time by keeping
			// a lookup table, but there shouldn't be that many USB storage devices connected...
			// NB: Each of these Device IDs have an _only_ child, from which we get the Device Instance match.
			for (device_id = devid_list; *device_id != 0; device_id += strlen(device_id) + 1) {
				if ( (CM_Locate_DevNodeA(&parent_inst, device_id, 0) == CR_SUCCESS)
				  && (CM_Get_Child(&device_inst, parent_inst, 0) == CR_SUCCESS)
				  && (device_inst == dev_info_data.DevInst) ) {
					// If we're not dealing with the USBSTOR part of our list, then this is an UASP device
					props.is_UASP = ((((uintptr_t)device_id)+2) >= ((uintptr_t)devid_list)+list_size[0]);
					// Now get the properties of the device, and its Device ID, which we need to populate the properties
					j = htab_hash(device_id, &htab_devid);
					if (j > 0) {
						GetUSBProperties(dev_if_path.String[(uint32_t)htab_devid.table[j].data], device_id, &props);
					}
				}
			}
		}
		if (props.is_VHD) {
			uprintf("Found VHD device '%s'\n", buffer);
		} else {
			if ((props.vid == 0) && (props.pid == 0)) {
				if (is_SCSI) {
					// If we have an SCSI drive and couldn't get a VID:PID, we are most likely
					// dealing with a system drive => eliminate it!
					continue;
				}
				safe_strcpy(str, sizeof(str), "????:????");	// Couldn't figure VID:PID
			} else {
				static_sprintf(str, "%04X:%04X", props.vid, props.pid);
			}
			if (props.speed >= USB_SPEED_MAX)
				props.speed = 0;
			uprintf("Found %s%s%s device '%s' (%s)\n", props.is_UASP?"UAS (":"", 
				usb_speed_name[props.speed], props.is_UASP?")":"", buffer, str);
			if (props.is_LowerSpeed)
				uprintf("NOTE: This device is an USB 3.0 device operating at lower speed...");
		}
		devint_data.cbSize = sizeof(devint_data);
		hDrive = INVALID_HANDLE_VALUE;
		devint_detail_data = NULL;
		for (j=0; ;j++) {
			safe_closehandle(hDrive);
			safe_free(devint_detail_data);

			if (!SetupDiEnumDeviceInterfaces(dev_info, &dev_info_data, &_GUID_DEVINTERFACE_DISK, j, &devint_data)) {
				if(GetLastError() != ERROR_NO_MORE_ITEMS) {
					uprintf("SetupDiEnumDeviceInterfaces failed: %s\n", WindowsErrorString());
				} else {
					uprintf("A device was eliminated because it didn't report itself as a disk\n");
				}
				break;
			}

			if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, NULL, 0, &size, NULL)) {
				if(GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
					devint_detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)calloc(1, size);
					if (devint_detail_data == NULL) {
						uprintf("Unable to allocate data for SP_DEVICE_INTERFACE_DETAIL_DATA\n");
						continue;
					}
					devint_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
				} else {
					uprintf("SetupDiGetDeviceInterfaceDetail (dummy) failed: %s\n", WindowsErrorString());
					continue;
				}
			}
			if (devint_detail_data == NULL) {
				uprintf("SetupDiGetDeviceInterfaceDetail (dummy) - no data was allocated\n");
				continue;
			}
			if(!SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, devint_detail_data, size, &size, NULL)) {
				uprintf("SetupDiGetDeviceInterfaceDetail (actual) failed: %s\n", WindowsErrorString());
				continue;
			}

			hDrive = CreateFileA(devint_detail_data->DevicePath, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			if(hDrive == INVALID_HANDLE_VALUE) {
				uprintf("Could not open '%s': %s\n", devint_detail_data->DevicePath, WindowsErrorString());
				continue;
			}

			drive_number = GetDriveNumber(hDrive, devint_detail_data->DevicePath);
			if (drive_number < 0)
				continue;

			drive_index = drive_number + DRIVE_INDEX_MIN;
			if (!IsMediaPresent(drive_index)) {
				uprintf("Device eliminated because it appears to contain no media\n");
				safe_closehandle(hDrive);
				safe_free(devint_detail_data);
				break;
			}

			if (GetDriveLabel(drive_index, drive_letters, &label)) {
				if ((!enable_HDDs) && (!props.is_VHD) &&
					((score = IsHDD(drive_index, (uint16_t)props.vid, (uint16_t)props.pid, buffer)) > 0)) {
					uprintf("Device eliminated because it was detected as an USB Hard Drive (score %d > 0)\n", score);
					uprintf("If this device is not an USB Hard Drive, please e-mail the author of this application\n");
					uprintf("NOTE: You can enable the listing of USB Hard Drives in 'Advanced Options' (after clicking the white triangle)");
					safe_closehandle(hDrive);
					safe_free(devint_detail_data);
					break;
				}

				// The empty string is returned for drives that don't have any volumes assigned
				if (drive_letters[0] == 0) {
					entry = lmprintf(MSG_046, label, drive_number,
						SizeToHumanReadable(GetDriveSize(drive_index), FALSE, use_fake_units));
				} else {
					// We have multiple volumes assigned to the same device (multiple partitions)
					// If that is the case, use "Multiple Volumes" instead of the label
					safe_strcpy(entry_msg, sizeof(entry_msg), ((drive_letters[0] != 0) && (drive_letters[1] != 0))?
						lmprintf(MSG_047):label);
					for (k=0; drive_letters[k]; k++) {
						// Append all the drive letters we detected
						letter_name[2] = drive_letters[k];
						if (right_to_left_mode)
							safe_strcat(entry_msg, sizeof(entry_msg), RIGHT_TO_LEFT_MARK);
						safe_strcat(entry_msg, sizeof(entry_msg), letter_name);
						if (drive_letters[k] == app_dir[0]) break;
					}
					// Repeat as we need to break the outside loop
					if (drive_letters[k] == app_dir[0]) {
						uprintf("Removing %c: from the list: This is the disk from which " APPLICATION_NAME " is running!\n", app_dir[0]);
						safe_closehandle(hDrive);
						safe_free(devint_detail_data);
						break;
					}
					safe_sprintf(&entry_msg[strlen(entry_msg)], sizeof(entry_msg) - strlen(entry_msg),
						"%s [%s]", (right_to_left_mode)?RIGHT_TO_LEFT_MARK:"", SizeToHumanReadable(GetDriveSize(drive_index), FALSE, use_fake_units));
					entry = entry_msg;
				}

				// Must ensure that the combo box is UNSORTED for indexes to be the same
				StrArrayAdd(&DriveID, buffer);
				StrArrayAdd(&DriveLabel, label);

				IGNORE_RETVAL(ComboBox_SetItemData(hDeviceList, ComboBox_AddStringU(hDeviceList, entry), drive_index));
				maxwidth = max(maxwidth, GetEntryWidth(hDeviceList, entry));
				safe_closehandle(hDrive);
				safe_free(devint_detail_data);
				break;
			}
		}
	}
	SetupDiDestroyDeviceInfoList(dev_info);

	// Adjust the Dropdown width to the maximum text size
	SendMessage(hDeviceList, CB_SETDROPPEDWIDTH, (WPARAM)maxwidth, 0);

	if (devnum >= DRIVE_INDEX_MIN) {
		for (i=0; i<ComboBox_GetCount(hDeviceList); i++) {
			if ((DWORD)ComboBox_GetItemData(hDeviceList, i) == devnum) {
				found = TRUE;
				break;
			}
		}
	}
	if (!found)
		i = 0;
	IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, i));
	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_DEVICE, 0);
	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
		ComboBox_GetCurSel(hFileSystem));
	r = TRUE;

out:
	safe_free(devid_list);
	StrArrayDestroy(&dev_if_path);
	htab_destroy(&htab_devid);
	return r;
}
