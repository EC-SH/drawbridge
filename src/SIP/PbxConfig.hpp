#ifndef PBX_CONFIG_HPP
#define PBX_CONFIG_HPP

// PbxConfig.hpp — per-extension call-forwarding and ring/hunt-group configuration
// for the "Class A" PBX feature sweep.
//
// Design mirrors the existing _dnd map (see RequestsHandler): a bounded
// std::unordered_map keyed by extension, guarded by the registrar's _mutex, and
// NVS-persisted so the config survives reboot. Like _dnd, the maps can never grow
// past the client-pool depth because the dashboard/API only ever sets config for
// real (validated) extensions, and clearing an entry erases it.
//
// All NVS access is gated on ESP_PLATFORM so the desktop gtest build stays
// host-compilable (on host the maps are pure RAM; load/persist are no-ops). The
// parsing helpers (parseReferToTarget, splitMembers) are pure and free-standing so
// the unit tests can exercise the routing logic without the full RequestsHandler.

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <cctype>

#include "PoolConfig.hpp"

namespace pbx
{
	// How a ring group fans an inbound INVITE out to its members.
	enum class GroupMode
	{
		RingAll,   // fork to every member at once; first to answer wins (like 999)
		Hunt       // ring members one at a time with a per-member timeout
	};

	// The three independent call-forward targets for one extension. An empty
	// string means "not configured" for that trigger. Stored value type.
	struct ForwardConfig
	{
		std::string always;     // CFU  — forward unconditionally, before ringing
		std::string busy;       // CFB  — forward when the callee returns 486 Busy
		std::string noAnswer;   // CFNA — forward when the callee never answers

		bool empty() const
		{
			return always.empty() && busy.empty() && noAnswer.empty();
		}
	};

	// One ring/hunt group: an ordered member list plus the fan-out mode.
	struct RingGroup
	{
		std::vector<std::string> members;
		GroupMode mode = GroupMode::RingAll;
	};

	// ── Pure parsing helpers (unit-tested directly) ───────────────────────────

	// Extract the target extension (AOR user-part) from a Refer-To header value.
	// Handles the common forms produced by SIP UAs, e.g.:
	//   Refer-To: <sip:200@host:5060>
	//   Refer-To: "Bob" <sip:200@host>;some=param
	//   Refer-To: sip:200@host
	// Returns the user-part ("200") or "" if no sip: URI / user-part is present.
	// Mirrors SipMessage::extractNumber but works on a header VALUE that may carry
	// a display name and angle brackets, and tolerates a missing '@'.
	inline std::string parseReferToTarget(const std::string& referTo)
	{
		auto sipPos = referTo.find("sip:");
		if (sipPos == std::string::npos)
		{
			return {};
		}
		size_t start = sipPos + 4;

		// The user-part ends at '@' (user@host) or, if there is no host part, at the
		// first URI delimiter: '>', ';', '?' or whitespace.
		size_t end = referTo.size();
		for (size_t i = start; i < referTo.size(); ++i)
		{
			char c = referTo[i];
			if (c == '@' || c == '>' || c == ';' || c == '?' ||
				c == ' ' || c == '\t' || c == '\r' || c == '\n')
			{
				end = i;
				break;
			}
		}
		if (end <= start)
		{
			return {};
		}
		return referTo.substr(start, end - start);
	}

	// Split a comma/whitespace-separated member list (as stored in NVS / posted from
	// the dashboard) into individual extensions, trimming surrounding whitespace and
	// dropping empties. Bounded by POCKETDIAL_MAX_CLIENTS so a malicious list can't
	// allocate without limit.
	inline std::vector<std::string> splitMembers(const std::string& csv)
	{
		std::vector<std::string> out;
		size_t i = 0;
		while (i < csv.size() && out.size() < static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			// Skip separators (comma or whitespace).
			while (i < csv.size() &&
				(csv[i] == ',' || std::isspace(static_cast<unsigned char>(csv[i]))))
			{
				++i;
			}
			size_t start = i;
			while (i < csv.size() && csv[i] != ',' &&
				!std::isspace(static_cast<unsigned char>(csv[i])))
			{
				++i;
			}
			if (i > start)
			{
				out.push_back(csv.substr(start, i - start));
			}
		}
		return out;
	}

	// Join a member list back into the canonical comma-separated form used for NVS
	// persistence and the dashboard snapshot.
	inline std::string joinMembers(const std::vector<std::string>& members)
	{
		std::string out;
		for (size_t i = 0; i < members.size(); ++i)
		{
			if (i) out.push_back(',');
			out += members[i];
		}
		return out;
	}
}

#endif
