/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file company_type.h Types related to companies. */

#ifndef COMPANY_TYPE_H
#define COMPANY_TYPE_H

#include "core/enum_type.hpp"
#include "core/pool_type.hpp"

using CompanyID = PoolID<uint8_t, struct CompanyIDTag, 0xF, 0xFF>;

/* 'Fake' companies used for networks */
static constexpr CompanyID COMPANY_INACTIVE_CLIENT{253}; ///< The client is joining
static constexpr CompanyID COMPANY_NEW_COMPANY{254}; ///< The client wants a new company
static constexpr CompanyID COMPANY_SPECTATOR{255}; ///< The client is spectating

using Owner = CompanyID;
static constexpr Owner OWNER_BEGIN = Owner::Begin(); ///< First owner
static constexpr Owner OWNER_TOWN{0x0F}; ///< A town owns the tile, or a town is expanding
static constexpr Owner OWNER_NONE{0x10}; ///< The tile has no ownership
static constexpr Owner OWNER_WATER{0x11}; ///< The tile/execution is done by "water"
static constexpr Owner OWNER_DEITY{0x12}; ///< The object is owned by a superuser / goal script
static constexpr Owner OWNER_END{0x13}; ///< Last + 1 owner
static constexpr Owner INVALID_OWNER = Owner::Invalid(); ///< An invalid owner

static const uint8_t MAX_COMPANIES = CompanyID::End().base();
static const uint MAX_LENGTH_PRESIDENT_NAME_CHARS = 32; ///< The maximum length of a president name in characters including '\0'
static const uint MAX_LENGTH_COMPANY_NAME_CHARS   = 32; ///< The maximum length of a company name in characters including '\0'

static const uint MAX_HISTORY_QUARTERS            = 24; ///< The maximum number of quarters kept as performance's history

static const uint MIN_COMPETITORS_INTERVAL = 0;   ///< The minimum interval (in minutes) between competitors.
static const uint MAX_COMPETITORS_INTERVAL = 500; ///< The maximum interval (in minutes) between competitors.

typedef Owner CompanyID;

class CompanyMask : public BaseBitSet<CompanyMask, CompanyID, uint16_t> {
public:
	constexpr CompanyMask() : BaseBitSet<CompanyMask, CompanyID, uint16_t>() {}
	static constexpr size_t DecayValueType(CompanyID value) { return value.base(); }

	constexpr auto operator <=>(const CompanyMask &) const noexcept = default;
};

struct Company;

struct CompanyManagerFace {
	uint style = 0; ///< Company manager face style.
	uint32_t bits = 0; ///< Company manager face bits, meaning is dependent on style.
	std::string style_label; ///< Face style label.
};

/** The reason why the company was removed. */
enum CompanyRemoveReason : uint8_t {
	CRR_MANUAL,    ///< The company is manually removed.
	CRR_AUTOCLEAN, ///< The company is removed due to autoclean.
	CRR_BANKRUPT,  ///< The company went belly-up.

	CRR_END,       ///< Sentinel for end.

	CRR_NONE = CRR_MANUAL, ///< Dummy reason for actions that don't need one.
};

/** The action to do with CMD_COMPANY_CTRL. */
enum CompanyCtrlAction : uint8_t {
	CCA_NEW,    ///< Create a new company.
	CCA_NEW_AI, ///< Create a new AI company.
	CCA_DELETE, ///< Delete a company.

	CCA_END,    ///< Sentinel for end.
};

/** The action to do with CMD_COMPANY_ALLOW_LIST_CTRL. */
enum CompanyAllowListCtrlAction : uint8_t {
	CALCA_ADD, ///< Create a public key.
	CALCA_REMOVE, ///< Remove a public key.

	CALCA_END,    ///< Sentinel for end.
};

#endif /* COMPANY_TYPE_H */
