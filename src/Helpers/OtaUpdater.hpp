#ifndef OTA_UPDATER_HPP
#define OTA_UPDATER_HPP

// OtaUpdater: a thin, portable wrapper over the ESP-IDF app_update API
// (esp_ota_ops.h / esp_partition.h) used by the HTTP dashboard to stream a new
// firmware image into the inactive OTA slot and activate it.
//
// Why this exists: the device ships a single `factory` app partition; Phase-1
// production hardening adds dual-OTA (ota_0 / ota_1 + otadata, see
// docs/OTA.md). This class hides the esp_ota_* state machine behind a tiny,
// testable surface and — crucially for this repo — compiles on the desktop
// (host) build too, where the ESP-IDF APIs do not exist. On host every method
// is a deterministic stub so the cross-compiled binary links and the host smoke
// tests stay green; no real flashing happens off-device.
//
// Lifecycle (device):
//     begin(totalSize)  -> esp_ota_get_next_update_partition + esp_ota_begin
//     write(data, len)  -> esp_ota_write   (call repeatedly while streaming)
//     end()             -> esp_ota_end     (finalizes + validates the image)
//     activate()        -> esp_ota_set_boot_partition (next boot uses new slot)
//     abort()           -> esp_ota_abort   (release the handle on any failure)
//
// Rollback strategy (see docs/OTA.md): with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y the freshly activated image boots in
// the "pending verify" state. The application confirms a healthy boot by calling
// markValid() (esp_ota_mark_app_valid_cancel_rollback). If the new image never
// reaches markValid() (crash/boot loop), the bootloader rolls back to the
// previous slot on the next reset.
//
// Thread-safety: a single OTA session is expected to run on one connection
// thread at a time (the HTTP server streams one upload per request). The class
// does not add its own locking; callers must not drive one instance from
// multiple threads concurrently. All shared state is per-instance.

#include <cstddef>
#include <cstdint>
#include <string>

class OtaUpdater
{
public:
	OtaUpdater() = default;
	~OtaUpdater();

	// Non-copyable / non-movable: it owns an esp_ota handle (an opaque
	// resource) for the duration of a session.
	OtaUpdater(const OtaUpdater&) = delete;
	OtaUpdater& operator=(const OtaUpdater&) = delete;
	OtaUpdater(OtaUpdater&&) = delete;
	OtaUpdater& operator=(OtaUpdater&&) = delete;

	// Open a write session against esp_ota_get_next_update_partition(NULL).
	// totalSize is the expected image size (may be OTA_SIZE_UNKNOWN/0 if not
	// known; we pass the Content-Length when we have it so the IDF can pre-erase
	// efficiently). Returns false (and sets lastError) on failure.
	// Host stub: records the size, marks the session active, returns true.
	bool begin(size_t totalSize);

	// Write the next chunk of image bytes. Must be called only after a
	// successful begin(). Returns false (and sets lastError) on failure.
	// Host stub: accumulates the byte count and discards the data.
	bool write(const uint8_t* data, size_t len);

	// Finalize the image: esp_ota_end() runs the integrity/signature checks.
	// Returns false (and sets lastError) on a corrupt/invalid image.
	// Host stub: returns true.
	bool end();

	// Set the just-written partition as the boot partition for the next reset.
	// Call only after a successful end(). Returns false on failure.
	// Host stub: marks an update pending and returns true.
	bool activate();

	// Release the open handle (esp_ota_abort) if a session is in progress.
	// Safe to call when no session is open. Host stub: clears the active flag.
	void abort();

	// True while begin() has succeeded and end()/abort() has not yet been called.
	bool isInProgress() const { return _inProgress; }

	// True once activate() has succeeded: a new image is staged and the device
	// should reboot to run it.
	bool isUpdatePending() const { return _updatePending; }

	// Human-readable last error (empty if none). Stable, caller-owned copy.
	const std::string& lastError() const { return _lastError; }

	// Total bytes accepted by write() so far in the current/last session.
	size_t bytesWritten() const { return _bytesWritten; }

	// --- Static status helpers (no session required) ---

	// Label of the partition the device is currently running from
	// (e.g. "ota_0"). Host: a fixed placeholder string.
	static std::string runningPartitionLabel();

	// Label of the partition a new image would be written to / will boot next
	// (esp_ota_get_next_update_partition). Host: a fixed placeholder string.
	static std::string nextUpdatePartitionLabel();

	// Label of the configured boot partition (esp_ota_get_boot_partition).
	// Host: a fixed placeholder string.
	static std::string bootPartitionLabel();

	// True iff the running image is in the "pending verify" state (i.e. it was
	// just OTA-activated and has not yet been confirmed via markValid()).
	// Host: always false.
	static bool isPendingVerify();

	// Confirm a healthy boot: cancels the pending rollback so the running image
	// becomes the permanent boot choice
	// (esp_ota_mark_app_valid_cancel_rollback). Returns false on failure.
	// Safe / idempotent to call when no verification is pending.
	// Host: no-op, returns true.
	static bool markValid();

	// --- Image signature verification (issue #47) ----------------------------
	//
	// esp_ota_end() only checks the image magic + per-segment checksum — NOT a
	// cryptographic signature — so on its own OTA accepts any well-formed image an
	// authenticated (or first-run) caller uploads, which is a remote-persistent-
	// compromise vector (THREAT_MODEL T-5). The durable fix is ESP-IDF Secure Boot
	// v2 (eFuse-burned key, bootloader-enforced) — a factory/key-management decision
	// that burns one-way eFuses and is out of scope for a pure code change.
	//
	// This is the application-layer half of that fix: as the image streams, write()
	// folds every byte into a SHA-256 (PSA Crypto on-device). The HTTP layer carries
	// a detached signature (the `X-OTA-Signature` request header: base64 of the raw
	// 64-byte ECDSA-P256 r||s signature over that SHA-256, made with the operator's
	// OTA private key). After end() and BEFORE activate(), the caller invokes
	// verifySignature() with the decoded signature; only a signature that verifies
	// against the embedded OTA PUBLIC key lets the image become the boot choice.
	//
	// Key-provisioning hook (documented in docs/OTA.md): the trusted public key is
	// supplied at build time via the PD_OTA_PUBLIC_KEY_HEX macro (hex of the 65-byte
	// uncompressed P-256 point). The SAME EC P-256 key can later be promoted to the
	// Secure Boot v2 signing key, so this path is forward-compatible with the
	// durable fix.

	// True iff this build enforces OTA image signatures. Controlled by the
	// PD_OTA_REQUIRE_SIGNATURE build macro. When true, the HTTP upload handler MUST
	// obtain a signature and call verifySignature() (a missing/failed signature
	// rejects the image). When false (default — preserves first-run onboarding and
	// the host CI smoke path, and avoids bricking units with no key provisioned),
	// the verification path still RUNS if a signature is supplied, but an absent
	// signature only logs a loud warning. Host: false.
	static bool signatureRequired();

	// True iff a trusted OTA public key is compiled in (PD_OTA_PUBLIC_KEY_PEM set).
	// Used to fail closed when enforcement is on but no key was provisioned. Host:
	// reflects the macro so the enforcement logic is host-testable.
	static bool publicKeyProvisioned();

	// Finalize the streamed image's SHA-256 and verify `signatureRaw` (the raw
	// 64-byte ECDSA-P256 r||s signature) against the embedded OTA public key. Returns
	// true ONLY on a cryptographically valid signature over exactly the bytes that
	// were write()-streamed in this session. Returns false (and sets lastError) on a
	// bad signature, a missing/malformed key, a wrong-length signature, or if no
	// bytes were written. Call after end(), before activate(). Host: returns false
	// with a clear error (no real crypto wired on host), mirroring device
	// fail-closed semantics for the enforcement unit tests.
	bool verifySignature(const std::string& signatureRaw);

private:
	bool        _inProgress    = false;
	bool        _updatePending = false;
	size_t      _expectedSize  = 0;
	size_t      _bytesWritten  = 0;
	std::string _lastError;
	bool        _hashInit      = false;   // image SHA-256 accumulator started?

#if defined(ESP_PLATFORM)
	// esp_ota_handle_t is a uint64 typedef in ESP-IDF; we store it type-erased to
	// keep esp_ota_ops.h out of this header (so host TUs never see it). The .cpp
	// reinterprets it back. 0 is the "no handle" sentinel.
	uint64_t    _otaHandle     = 0;
	// const esp_partition_t* of the slot we are writing to. Opaque here.
	const void* _updatePart    = nullptr;
	// Opaque mbedtls_sha256_context*, heap-allocated in begin() so esp/mbedtls
	// headers stay out of this header (host TUs never see them). Freed in
	// abort()/end()/dtor. nullptr when no session-hash is active.
	void*       _hashCtx       = nullptr;
#endif
};

#endif // OTA_UPDATER_HPP
