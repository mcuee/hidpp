/*
 * Copyright 2017 Clément Vuchener
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
 *
 */

#include "RawDevice.h"

#include <misc/Log.h>

#include <stdexcept>
#include <locale>
#include <codecvt>
#include <array>
#include <map>
#include <set>
#include <cstring>
#include <cassert>

extern "C" {
#include <windows.h>
#include <winbase.h>
#include <hidsdi.h>
}

#include "windows/error_category.h"
#include "windows/DeviceData.h"

#define ARRAY_SIZE(a) (sizeof (a)/sizeof ((a)[0]))

class RAII_HANDLE
{
	HANDLE _handle;

public:
	RAII_HANDLE ():
		_handle (INVALID_HANDLE_VALUE)
	{
	}

	RAII_HANDLE (HANDLE handle):
		_handle (handle)
	{
	}

	RAII_HANDLE (RAII_HANDLE &&other):
		_handle (other._handle)
	{
		other._handle = INVALID_HANDLE_VALUE;
	}

	~RAII_HANDLE ()
	{
		CloseHandle (_handle);
	}

	RAII_HANDLE &operator= (HANDLE handle)
	{
		if (_handle != INVALID_HANDLE_VALUE)
			CloseHandle (_handle);
		_handle = handle;
		return *this;
	}

	RAII_HANDLE &operator= (RAII_HANDLE &&handle)
	{
		std::swap (*this, handle);
		return *this;
	}

	inline operator HANDLE () const
	{
		return _handle;
	}
};

using namespace HID;

struct RawDevice::PrivateImpl
{
	struct Device
	{
		RAII_HANDLE file;
		RAII_HANDLE event;
		HIDP_CAPS caps;
	};
	std::vector<Device> devices;
	std::map<uint8_t, HANDLE> reports;
	RAII_HANDLE interrupted_event;
};

RawDevice::RawDevice ():
	_p (std::make_unique<PrivateImpl> ())
{
}

RawDevice::RawDevice (const std::string &path):
	_p (std::make_unique<PrivateImpl> ())
{
	std::wstring_convert<std::codecvt_utf8<wchar_t, 0x10ffff, std::little_endian>> wconv;
	std::wstring_convert<std::codecvt_utf8<char16_t>, char16_t> u16conv;

	std::wstring parent_id = wconv.from_bytes (path);

	DWORD err;

	GUID hid_guid;
	HidD_GetHidGuid (&hid_guid);

	bool first = true;
	DeviceEnumerator enumerator (&hid_guid);
	DEVINST parent_inst = DeviceData (enumerator.devinfo (), parent_id.c_str ()).deviceInst ();
	int i = 0;
	while (auto dev = enumerator.get (i++)) {
		bool ok;
		DEVINST parent;
		std::tie (ok, parent) = dev->parentInst ();
		if (!ok || parent != parent_inst)
			continue;

		HANDLE hdev = CreateFile (dev->devicePath (),
					  GENERIC_READ | GENERIC_WRITE,
					  FILE_SHARE_READ | FILE_SHARE_WRITE,
					  NULL, OPEN_EXISTING,
					  FILE_FLAG_OVERLAPPED, NULL);
		if (hdev == INVALID_HANDLE_VALUE) {
			err = GetLastError ();
			Log::debug () << "Failed to open device "
				      << wconv.to_bytes (dev->devicePath ()) << ": "
				      << windows_category ().message (err) << std::endl;
			continue;
		}

		HANDLE event = CreateEvent (NULL, TRUE, TRUE, NULL);
		if (event == INVALID_HANDLE_VALUE) {
			err = GetLastError ();
			throw std::system_error (err, windows_category (),
						 "CreateEvent");
		}

		_p->devices.push_back ({hdev, event});

		if (first) {
			HIDD_ATTRIBUTES attrs;
			if (!HidD_GetAttributes (hdev, &attrs)) {
				err = GetLastError ();
				throw std::system_error (err, windows_category (),
							 "HidD_GetAttributes");
			}
			_vendor_id = attrs.VendorID;
			_product_id = attrs.ProductID;

			char16_t buffer[256];
			if (!HidD_GetManufacturerString (hdev, buffer, ARRAY_SIZE (buffer))) {
				err = GetLastError ();
				throw std::system_error (err, windows_category (),
							 "HidD_GetManufacturerString");
			}
			_name = u16conv.to_bytes (buffer);
			if (!HidD_GetProductString (hdev, buffer, ARRAY_SIZE (buffer))) {
				err = GetLastError ();
				throw std::system_error (err, windows_category (),
							 "HidD_GetProductString");
			}
			_name += " " + u16conv.to_bytes (buffer);
			first = false;
		}

		PHIDP_PREPARSED_DATA preparsed_data;
		if (!HidD_GetPreparsedData (hdev, &preparsed_data)) {
			err = GetLastError ();
			throw std::system_error (err, windows_category (),
						 "HidD_GetPreparsedData");
		}

		std::unique_ptr<_HIDP_PREPARSED_DATA, decltype(&HidD_FreePreparsedData)> unique_preparsed_data (preparsed_data, &HidD_FreePreparsedData);

		HIDP_CAPS &caps = _p->devices.back ().caps;
		if (HIDP_STATUS_SUCCESS != HidP_GetCaps (preparsed_data, &caps))
			throw std::runtime_error ("HidP_GetCaps failed");

		auto &collection = _report_desc.collections.emplace_back();
		collection.usage = (uint32_t (caps.UsagePage) << 16) | caps.Usage;

		for (auto [windows_report_type, report_type, button_count, value_count]: {
				std::make_tuple(HidP_Input, ReportID::Type::Input, caps.NumberInputButtonCaps, caps.NumberInputValueCaps),
				std::make_tuple(HidP_Output, ReportID::Type::Output, caps.NumberOutputButtonCaps, caps.NumberOutputValueCaps),
				std::make_tuple(HidP_Feature, ReportID::Type::Feature, caps.NumberFeatureButtonCaps, caps.NumberFeatureValueCaps)}) {
			std::vector<HIDP_BUTTON_CAPS> button_caps (button_count);
			std::vector<HIDP_VALUE_CAPS> value_caps (value_count);

			USHORT len = button_caps.size ();
			if (len > 0 && HIDP_STATUS_SUCCESS != HidP_GetButtonCaps (windows_report_type, button_caps.data (), &len, preparsed_data))
				throw std::runtime_error ("HidP_GetButtonCaps failed");
			len = value_caps.size ();
			if (len > 0 && HIDP_STATUS_SUCCESS != HidP_GetValueCaps (windows_report_type, value_caps.data (), &len, preparsed_data))
				throw std::runtime_error ("HidP_GetValueCaps failed");

			auto add_field = [&] (auto cap) {
				auto [it, inserted] = collection.reports.emplace (ReportID{report_type, cap.ReportID}, 0);
				auto &f = it->second.emplace_back ();
				f.flags.bits = cap.BitField;
				if (cap.IsRange) {
					f.usages = std::make_pair (
							(uint32_t (cap.UsagePage) << 16) | cap.Range.UsageMin,
							(uint32_t (cap.UsagePage) << 16) | cap.Range.UsageMax);
				}
				else {
					f.usages = std::vector {(uint32_t (cap.UsagePage) << 16) | cap.NotRange.Usage};
				}
			};

			// Add fields ordered by DataIndex
			auto button_it = button_caps.begin ();
			auto value_it = value_caps.begin ();
			while (button_it != button_caps.end () && value_it != value_caps.end ()) {
				if (button_it->NotRange.DataIndex < value_it->NotRange.DataIndex)
					add_field (*(button_it++));
				else
					add_field (*(value_it++));
			}
			while (button_it != button_caps.end ())
				add_field (*(button_it++));
			while (value_it != value_caps.end ())
				add_field (*(value_it++));
		}

		for (const auto &[id, fields]: collection.reports) {
			auto [it, inserted] = _p->reports.emplace (id.id, hdev);
			if (!inserted && it->second != hdev)
				throw std::runtime_error ("Same Report ID on different handle.");
		}
	}

	_p->interrupted_event = CreateEvent (NULL, FALSE, FALSE, NULL);
	if (_p->interrupted_event == INVALID_HANDLE_VALUE) {
		err = GetLastError ();
		throw std::system_error (err, windows_category (),
					 "CreateEvent");
	}
}

RawDevice::RawDevice (const RawDevice &other):
	_p (std::make_unique<PrivateImpl> ()),
	_vendor_id (other._vendor_id), _product_id (other._product_id),
	_name (other._name),
	_report_desc (other._report_desc)
{
	DWORD err;

	for (const auto &dev: other._p->devices) {
		HANDLE hdev = ReOpenFile (dev.file,
					  GENERIC_READ | GENERIC_WRITE,
					  FILE_SHARE_READ | FILE_SHARE_WRITE,
					  FILE_FLAG_OVERLAPPED);
		if (hdev == INVALID_HANDLE_VALUE) {
			err = GetLastError ();
			throw std::system_error (err, windows_category (),
						 "ReOpenFile");
		}

		HANDLE event = CreateEvent (NULL, TRUE, TRUE, NULL);
		if (event == INVALID_HANDLE_VALUE) {
			err = GetLastError ();
			throw std::system_error (err, windows_category (),
						 "CreateEvent");
		}

		_p->devices.push_back ({hdev, event});
	}

	_p->interrupted_event = CreateEvent (NULL, FALSE, FALSE, NULL);
	if (_p->interrupted_event == INVALID_HANDLE_VALUE) {
		err = GetLastError ();
		throw std::system_error (err, windows_category (),
					 "CreateEvent");
	}
}

RawDevice::RawDevice (RawDevice &&other):
	_p (std::make_unique<PrivateImpl> ()),
	_vendor_id (other._vendor_id), _product_id (other._product_id),
	_name (std::move (other._name)),
	_report_desc (std::move (other._report_desc))
{
	std::swap (_p, other._p);
}

RawDevice::~RawDevice ()
{
}

int RawDevice::writeReport (const std::vector<uint8_t> &report)
{
	DWORD err, written;
	OVERLAPPED overlapped;
	memset (&overlapped, 0, sizeof (OVERLAPPED));
	auto it = _p->reports.find (report[0]);
	if (it == _p->reports.end ())
		throw std::runtime_error ("Report ID not found.");
	if (!WriteFile (it->second, report.data (), report.size (),
			&written, &overlapped)) {
		err = GetLastError ();
		if (err == ERROR_IO_PENDING) {
			if (!GetOverlappedResult (it->second, &overlapped,
						  &written, TRUE)) {
				err = GetLastError ();
				throw std::system_error (err, windows_category (),
							 "GetOverlappedResult");
			}
		}
		else
			throw std::system_error (err, windows_category (), "WriteFile");
	}
	Log::debug ("report").printBytes ("Send HID report:", report.begin (), report.end ());
	return written;
}

class AsyncRead
{
	HANDLE file;
	OVERLAPPED overlapped;
	bool pending;

public:
	AsyncRead (HANDLE file, HANDLE event):
		file (file),
		pending (false)
	{
		memset (&overlapped, 0, sizeof (OVERLAPPED));
		overlapped.hEvent = event;
	}

	bool read (LPVOID buffer, DWORD size, LPDWORD read)
	{
		DWORD err;
		if (ReadFile (file, buffer, size, read, &overlapped))
			return pending = false;
		err = GetLastError ();
		if (err == ERROR_IO_PENDING)
			return pending = true;
		else
			throw std::system_error (err, windows_category (),
						 "ReadFile");
	}

	void finish (LPDWORD read)
	{
		DWORD err;
		pending = false;
		if (!GetOverlappedResult (file, &overlapped, read, FALSE)) {
			err = GetLastError ();
			throw std::system_error (err, windows_category (),
						 "GetOverlappedResult");
		}
	}

	~AsyncRead ()
	{
		DWORD err;
		if (pending && !CancelIoEx (file, &overlapped)) {
			err = GetLastError ();
			Log::error () << "Failed to cancel async read: "
				      << windows_category ().message (err)
				      << std::endl;
		}
	}
};

int RawDevice::readReport (std::vector<uint8_t> &report, int timeout)
{
	DWORD err, read, ret, i;
	assert (_p->interrupted_event != INVALID_HANDLE_VALUE);
	std::vector<HANDLE> handles = { _p->interrupted_event };
	std::vector<AsyncRead> reads; // vector destructor will automatically cancel pending IOs.
	reads.reserve (_p->devices.size ()); // Reserve memory so overlapped are not moved.

	for (auto &dev: _p->devices) {
		if (report.size () < dev.caps.InputReportByteLength)
			continue; // skip device with reports that would not fit in the buffer
		reads.emplace_back (dev.file, dev.event);
		if (!reads.back ().read (report.data (), report.size (), &read))
			goto report_read;
		handles.push_back (dev.event);
	}
	ret = WaitForMultipleObjects (handles.size (), handles.data (), FALSE,
				      (timeout < 0 ? INFINITE : timeout));
	switch (ret) {
	case WAIT_OBJECT_0: // interrupted
	case WAIT_TIMEOUT:
		return 0;
	case WAIT_FAILED:
		err = GetLastError ();
		throw std::system_error (err, windows_category (), "WaitForMultipleObject");
	default:
		i = ret-WAIT_OBJECT_0-1;
		if (i < 0 || i >= reads.size ())
			throw std::runtime_error ("Unexpected return value from WaitForMultipleObject");
		else
			reads[i].finish (&read);
	}
report_read:
	report.resize (read);
	Log::debug ("report").printBytes ("Recv HID report:", report.begin (), report.end ());
	return read;
}

void RawDevice::interruptRead ()
{
	DWORD err;
	if (!SetEvent (_p->interrupted_event)) {
		err = GetLastError ();
		throw std::system_error (err, windows_category (), "SetEvent");
	}
}
