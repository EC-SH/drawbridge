#ifndef POCKETDIAL_URL_ENCODE_HPP
#define POCKETDIAL_URL_ENCODE_HPP

#include <string>

// RFC 3986 percent-encoding for a single URL path/query component. The unreserved
// set (A-Z a-z 0-9 - _ . ~) passes through unchanged; every other byte is emitted
// as %XX with uppercase hex. Bytes are treated as unsigned so non-ASCII encodes
// correctly. Pure and host-compilable (no ESP/socket dependencies) so it is
// unit-tested in the host suite (tests/UrlEncode_test.cpp) and reused by the ESP
// 3CX anchor when building device-specific makecall URLs from a device_id that may
// carry reserved characters.
inline std::string urlEncode(const std::string& in)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(in.size());
	for (unsigned char c : in)
	{
		const bool unreserved =
			(c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved)
		{
			out.push_back(static_cast<char>(c));
		}
		else
		{
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

#endif // POCKETDIAL_URL_ENCODE_HPP
