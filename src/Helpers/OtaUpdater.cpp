// OtaUpdater.cpp — see OtaUpdater.hpp for the design notes and lifecycle.
//
// Two implementations live side by side, selected by the platform guard:
//   * ESP_PLATFORM: a thin wrapper over the ESP-IDF app_update component
//     (esp_ota_ops.h / esp_partition.h). Every esp_* call is error-checked and
//     surfaced through _lastError; we never abort()/restart from inside here.
//   * Host (desktop Linux / CI): deterministic stubs with identical signatures
//     so the host binary links and behaves predictably for the smoke tests.

#include "OtaUpdater.hpp"

#if defined(ESP_PLATFORM)
#include "esp_ota_ops.h"   // esp_ota_*; esp_ota_img_states_t / ESP_OTA_IMG_* enums
#include "esp_partition.h" // esp_partition_t
#include "esp_err.h"       // esp_err_t, esp_err_to_name
#include "esp_log.h"

// ESP-IDF v6 ships mbedTLS 4.x: the legacy mbedtls/{sha256,pk}.h headers are no
// longer public (they moved under .../private). The supported on-device crypto API
// is PSA Crypto (psa/crypto.h) — the same API the littlessh SSH backend uses. We do
// the image SHA-256 + ECDSA-P256 verify through PSA.
#include "psa/crypto.h"

#include <cstring>
#include <vector>

// Trusted OTA signing PUBLIC key (issue #47 key-provisioning hook). Supply it at
// build time as the HEX of the 65-byte uncompressed P-256 point (0x04 || X || Y),
// e.g. in main/CMakeLists.txt:
//   target_compile_definitions(${COMPONENT_LIB} PRIVATE
//     PD_OTA_REQUIRE_SIGNATURE
//     PD_OTA_PUBLIC_KEY_HEX="04ab...ef")   # 130 hex chars
// Generate the keypair, derive the raw public point, and sign an image's SHA-256:
//   openssl ecparam -name prime256v1 -genkey -noout -out ota_signing.pem
//   openssl ec -in ota_signing.pem -text -noout            # copy the "pub:" bytes
//   openssl dgst -sha256 -sign ota_signing.pem -out img.der.sig build/SipServer.bin
//   # X-OTA-Signature is base64 of the RAW r||s (64 bytes). Convert the DER sig:
//   #   (see docs/OTA.md §6 for the der->raw one-liner)
// The same EC P-256 key can later be promoted to the Secure Boot v2 key, so this
// app-layer check is forward-compatible with the durable fix.
#if !defined(PD_OTA_PUBLIC_KEY_HEX)
#define PD_OTA_PUBLIC_KEY_HEX ""
#endif

namespace
{
	const char TAG_OTA[] = "OtaUpdater";

	// Reinterpret the type-erased handle stored in the header back to the real
	// IDF type. esp_ota_handle_t is an unsigned integer typedef.
	inline esp_ota_handle_t toHandle(uint64_t h) { return static_cast<esp_ota_handle_t>(h); }

	std::string partLabel(const esp_partition_t* p)
	{
		return (p != nullptr) ? std::string(p->label) : std::string("(none)");
	}

	// The compiled-in OTA public key, as hex (empty when none was provisioned).
	const char* otaPublicKeyHex() { return PD_OTA_PUBLIC_KEY_HEX; }
	bool        otaPublicKeyPresent() { return otaPublicKeyHex()[0] != '\0'; }

	// Decode a hex string to raw bytes. Returns false on odd length / non-hex.
	bool hexDecode(const char* hex, std::vector<uint8_t>& out)
	{
		auto nib = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		size_t n = std::strlen(hex);
		if (n == 0 || (n % 2) != 0) return false;
		out.clear();
		out.reserve(n / 2);
		for (size_t i = 0; i < n; i += 2)
		{
			int hi = nib(hex[i]);
			int lo = nib(hex[i + 1]);
			if (hi < 0 || lo < 0) return false;
			out.push_back(static_cast<uint8_t>((hi << 4) | lo));
		}
		return true;
	}

	psa_hash_operation_t* asHash(void* p) { return static_cast<psa_hash_operation_t*>(p); }
}

OtaUpdater::~OtaUpdater()
{
	// Defensive: never leave an esp_ota handle dangling if the object is
	// destroyed mid-session (e.g. the connection thread unwinds on an error).
	abort();
}

bool OtaUpdater::begin(size_t totalSize)
{
	if (_inProgress)
	{
		_lastError = "OTA session already in progress";
		return false;
	}

	_bytesWritten  = 0;
	_expectedSize  = totalSize;
	_updatePending = false;
	_lastError.clear();

	const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
	if (part == nullptr)
	{
		_lastError = "no OTA update partition available (check partition table)";
		return false;
	}

	// Pass the known size so the IDF can erase exactly what it needs; if we don't
	// know it, OTA_SIZE_UNKNOWN makes esp_ota_begin erase the whole slot lazily.
	const size_t imageSize = (totalSize > 0) ? totalSize : OTA_SIZE_UNKNOWN;

	esp_ota_handle_t handle = 0;
	esp_err_t err = esp_ota_begin(part, imageSize, &handle);
	if (err != ESP_OK)
	{
		_lastError = std::string("esp_ota_begin failed: ") + esp_err_to_name(err);
		return false;
	}

	_updatePart = part;
	_otaHandle  = static_cast<uint64_t>(handle);
	_inProgress = true;

	// Start the image SHA-256 accumulator (issue #47) via PSA Crypto. Allocate the
	// op off the heap so the header stays crypto-free. psa_crypto_init() is
	// idempotent (cheap to call again — littlessh may already have inited it). A
	// hashing failure here is not fatal to the WRITE path (the image still flashes),
	// but verifySignature() then fails closed because _hashInit stays false.
	_hashCtx  = nullptr;
	_hashInit = false;
	if (psa_crypto_init() != PSA_SUCCESS)
	{
		ESP_LOGW(TAG_OTA, "psa_crypto_init failed; OTA signature verify unavailable");
		return true;
	}
	if (auto* op = new (std::nothrow) psa_hash_operation_t)
	{
		*op = PSA_HASH_OPERATION_INIT;
		if (psa_hash_setup(op, PSA_ALG_SHA_256) == PSA_SUCCESS)
		{
			_hashCtx  = op;
			_hashInit = true;
		}
		else
		{
			delete op;
			ESP_LOGW(TAG_OTA, "image SHA-256 setup failed; signature verify unavailable");
		}
	}
	return true;
}

bool OtaUpdater::write(const uint8_t* data, size_t len)
{
	if (!_inProgress)
	{
		_lastError = "OTA write before begin()";
		return false;
	}
	if (data == nullptr || len == 0)
	{
		// Treat an empty chunk as a no-op success (e.g. a 0-byte recv slice).
		return true;
	}

	esp_err_t err = esp_ota_write(toHandle(_otaHandle), data, len);
	if (err != ESP_OK)
	{
		_lastError = std::string("esp_ota_write failed: ") + esp_err_to_name(err);
		return false;
	}

	// Fold the SAME bytes into the image hash (issue #47). If the hash update fails,
	// invalidate the accumulator so a later verifySignature() fails closed rather
	// than checking a partial digest.
	if (_hashInit && _hashCtx != nullptr)
	{
		if (psa_hash_update(asHash(_hashCtx), data, len) != PSA_SUCCESS)
		{
			psa_hash_abort(asHash(_hashCtx));
			delete asHash(_hashCtx);
			_hashCtx  = nullptr;
			_hashInit = false;
			ESP_LOGW(TAG_OTA, "image SHA-256 update failed; signature verify will fail closed");
		}
	}

	_bytesWritten += len;
	return true;
}

bool OtaUpdater::end()
{
	if (!_inProgress)
	{
		_lastError = "OTA end() without an active session";
		return false;
	}

	esp_err_t err = esp_ota_end(toHandle(_otaHandle));
	// Whether end() succeeds or fails, the handle is consumed by esp_ota_end.
	_inProgress = false;
	_otaHandle  = 0;

	if (err != ESP_OK)
	{
		if (err == ESP_ERR_OTA_VALIDATE_FAILED)
			_lastError = "image validation failed (corrupt or wrong magic)";
		else
			_lastError = std::string("esp_ota_end failed: ") + esp_err_to_name(err);
		return false;
	}
	return true;
}

bool OtaUpdater::activate()
{
	if (_inProgress)
	{
		_lastError = "activate() called before end()";
		return false;
	}
	const esp_partition_t* part = static_cast<const esp_partition_t*>(_updatePart);
	if (part == nullptr)
	{
		_lastError = "no written partition to activate";
		return false;
	}

	esp_err_t err = esp_ota_set_boot_partition(part);
	if (err != ESP_OK)
	{
		_lastError = std::string("esp_ota_set_boot_partition failed: ") + esp_err_to_name(err);
		return false;
	}

	_updatePending = true;
	return true;
}

void OtaUpdater::abort()
{
	if (_inProgress && _otaHandle != 0)
	{
		// Best-effort: release the handle so the slot can be reused. Ignore the
		// return; there is nothing actionable on an abort failure.
		esp_ota_abort(toHandle(_otaHandle));
	}
	_inProgress = false;
	_otaHandle  = 0;

	// Free the image-hash accumulator if a session was torn down without a
	// verifySignature() (which would have freed it). Idempotent.
	if (_hashCtx != nullptr)
	{
		psa_hash_abort(asHash(_hashCtx));
		delete asHash(_hashCtx);
		_hashCtx = nullptr;
	}
	_hashInit = false;
}

bool OtaUpdater::signatureRequired()
{
#if defined(PD_OTA_REQUIRE_SIGNATURE)
	return true;
#else
	return false;
#endif
}

bool OtaUpdater::publicKeyProvisioned()
{
	return otaPublicKeyPresent();
}

bool OtaUpdater::verifySignature(const std::string& signatureRaw)
{
	// Take ownership of the hash op for this call regardless of outcome, so we never
	// leak it and a second call can't double-finalize.
	void* hashCtx = _hashCtx;
	_hashCtx = nullptr;
	bool   hadHash = _hashInit;
	_hashInit = false;

	struct CtxGuard
	{
		void* p;
		~CtxGuard()
		{
			if (p)
			{
				psa_hash_abort(static_cast<psa_hash_operation_t*>(p));
				delete static_cast<psa_hash_operation_t*>(p);
			}
		}
	} guard{hashCtx};

	if (!otaPublicKeyPresent())
	{
		// No trusted key compiled in: cannot verify. Caller (HTTP layer) decides
		// whether that is fatal based on signatureRequired() — keep the contract
		// explicit here by failing.
		_lastError = "no OTA public key provisioned (cannot verify signature)";
		return false;
	}
	if (!hadHash || hashCtx == nullptr)
	{
		_lastError = "image hash unavailable (no bytes hashed); rejecting signature";
		return false;
	}
	// PSA verify_hash for ECDSA-P256 expects the raw 64-byte r||s signature.
	if (signatureRaw.size() != 64)
	{
		_lastError = "OTA signature must be 64 raw bytes (P-256 r||s)";
		return false;
	}

	uint8_t digest[32];
	size_t digestLen = 0;
	if (psa_hash_finish(static_cast<psa_hash_operation_t*>(hashCtx),
	                    digest, sizeof(digest), &digestLen) != PSA_SUCCESS ||
	    digestLen != sizeof(digest))
	{
		_lastError = "failed to finalize image SHA-256";
		return false;
	}

	std::vector<uint8_t> pub;
	if (!hexDecode(otaPublicKeyHex(), pub) || pub.empty())
	{
		_lastError = "OTA public key (hex) malformed";
		return false;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	psa_key_id_t keyId = 0;
	psa_status_t st = psa_import_key(&attr, pub.data(), pub.size(), &keyId);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS)
	{
		_lastError = "OTA public key import failed";
		ESP_LOGE(TAG_OTA, "psa_import_key st=%d", static_cast<int>(st));
		return false;
	}

	st = psa_verify_hash(keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
	                     digest, sizeof(digest),
	                     reinterpret_cast<const uint8_t*>(signatureRaw.data()),
	                     signatureRaw.size());
	psa_destroy_key(keyId);

	if (st != PSA_SUCCESS)
	{
		_lastError = "OTA image signature verification FAILED";
		ESP_LOGE(TAG_OTA, "psa_verify_hash st=%d — rejecting image", static_cast<int>(st));
		return false;
	}

	ESP_LOGI(TAG_OTA, "OTA image signature verified");
	return true;
}

std::string OtaUpdater::runningPartitionLabel()
{
	return partLabel(esp_ota_get_running_partition());
}

std::string OtaUpdater::nextUpdatePartitionLabel()
{
	return partLabel(esp_ota_get_next_update_partition(nullptr));
}

std::string OtaUpdater::bootPartitionLabel()
{
	return partLabel(esp_ota_get_boot_partition());
}

bool OtaUpdater::isPendingVerify()
{
	const esp_partition_t* running = esp_ota_get_running_partition();
	if (running == nullptr) return false;

	esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
	if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
	return state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool OtaUpdater::markValid()
{
	esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
	return err == ESP_OK;
}

#else // ---------------------------------------------------------------- HOST

// Host stubs: no flash, no esp_ota_*. We simulate the state machine so the
// desktop binary links and behaves deterministically. No real update occurs.

OtaUpdater::~OtaUpdater()
{
	abort();
}

bool OtaUpdater::begin(size_t totalSize)
{
	if (_inProgress)
	{
		_lastError = "OTA session already in progress";
		return false;
	}
	_bytesWritten  = 0;
	_expectedSize  = totalSize;
	_updatePending = false;
	_inProgress    = true;
	_lastError.clear();
	return true;
}

bool OtaUpdater::write(const uint8_t* data, size_t len)
{
	if (!_inProgress)
	{
		_lastError = "OTA write before begin()";
		return false;
	}
	(void)data; // host: discard the bytes, just account for them
	_bytesWritten += len;
	return true;
}

bool OtaUpdater::end()
{
	if (!_inProgress)
	{
		_lastError = "OTA end() without an active session";
		return false;
	}
	_inProgress = false;
	return true;
}

bool OtaUpdater::activate()
{
	if (_inProgress)
	{
		_lastError = "activate() called before end()";
		return false;
	}
	_updatePending = true;
	return true;
}

void OtaUpdater::abort()
{
	_inProgress = false;
	_hashInit   = false;
}

std::string OtaUpdater::runningPartitionLabel()  { return "host"; }
std::string OtaUpdater::nextUpdatePartitionLabel() { return "host-sim"; }
std::string OtaUpdater::bootPartitionLabel()     { return "host"; }
bool        OtaUpdater::isPendingVerify()         { return false; }
bool        OtaUpdater::markValid()               { return true; }

// Host has no embedded OTA key and no real flash, so signing enforcement is OFF and
// no key is provisioned. These mirror the device contract so the HTTP enforcement
// LOGIC (require? key present? -> reject vs warn) is exercised identically on host.
#if defined(PD_OTA_REQUIRE_SIGNATURE)
bool OtaUpdater::signatureRequired() { return true; }
#else
bool OtaUpdater::signatureRequired() { return false; }
#endif

bool OtaUpdater::publicKeyProvisioned() { return false; }

bool OtaUpdater::verifySignature(const std::string& signatureRaw)
{
	(void)signatureRaw;
	// No crypto / no key on host: cannot affirmatively verify. Fail closed so a host
	// build can never report a real "signature OK".
	_hashInit  = false;
	_lastError = "signature verification unavailable on host build";
	return false;
}

#endif // ESP_PLATFORM
