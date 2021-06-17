/*
 * Copyright 2021 Clément Vuchener
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

#ifndef LIBHIDPP_HID_REPORT_DESCRIPTOR_H
#define LIBHIDPP_HID_REPORT_DESCRIPTOR_H

#include <cstdint>
#include <map>
#include <variant>
#include <vector>

namespace HID
{

struct ReportID
{
	enum class Type {
		Input = 8,
		Output = 9,
		Feature = 11,
	} type;
	unsigned int id;

	inline bool operator== (const ReportID &other) const noexcept {
		return std::make_tuple (type, id) == std::make_tuple (other.type, other.id);
	}
	inline bool operator!= (const ReportID &other) const noexcept {
		return std::make_tuple (type, id) != std::make_tuple (other.type, other.id);
	}
	inline bool operator< (const ReportID &other) const noexcept {
		return std::make_tuple (type, id) < std::make_tuple (other.type, other.id);
	}
};

struct ReportField
{
	struct Flags {
		unsigned int bits;
		enum Bits {
			Data_Constant = 1<<0,
			Array_Variable = 1<<1,
			Absolute_Relative = 1<<2,
			NoWrap_Wrap = 1<<3,
			Linear_NonLinear = 1<<4,
			PreferredState_NoPreferred = 1<<5,
			NoNullPosition_NullState = 1<<6,
			NonVolatile_Volatile = 1<<7,
			BitField_BufferedBytes = 1<<8,
		};
		bool isData () const noexcept { return !(bits & Data_Constant); }
		bool isConstant () const noexcept { return bits & Data_Constant; }
		bool isArray () const noexcept { return !(bits & Array_Variable); }
		bool isVariable () const noexcept { return bits & Array_Variable; }
	} flags;
	std::variant<
		std::vector<uint32_t>, // usage list
		std::pair<uint32_t, uint32_t> // usage range
		> usages;

};

struct ReportCollection
{
	enum class Type {
		Physical = 0,
		Application = 1,
		Logical = 2,
		Report = 3,
		NamedArray = 4,
		UsageSwitch = 5,
		UsageModifier = 6,
	} type;
	uint32_t usage;
	std::map<ReportID, std::vector<ReportField>> reports;
};

struct ReportDescriptor
{
	std::vector<ReportCollection> collections; // only top-level

	static ReportDescriptor fromRawData (const uint8_t *data, std::size_t length);
};

}

#endif
