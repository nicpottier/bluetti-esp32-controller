// Bluetti V2 BLE encryption handshake.
//
// Protocol (ported from bluetti-bt-lib / nhurman's bluetti_mqtt work):
//  1. Device sends plaintext "**" challenge with a 4-byte seed.
//     iv = md5(reverse(seed)), unsecure_key = iv ^ LOCAL_AES_KEY (AES-128).
//     We reply "**" 02 04 iv[8..12] + checksum.
//  2. Device sends (AES-128-CBC, fixed iv) its P-256 public key, signed with K2.
//     We reply with our ephemeral public key, signed with L1.
//  3. Device confirms; secure key = ECDH shared secret x (AES-256).
//     From then on every frame is len(2) + ivSeed(4) + AES-256-CBC(md5(ivSeed)).
#pragma once

#include <Arduino.h>
#include <vector>

using Bytes = std::vector<uint8_t>;

class BluettiCrypto {
 public:
  enum class Result { None, Reply, Ready, Data, Error };

  BluettiCrypto();
  ~BluettiCrypto();

  void reset();
  bool ready() const { return haveSecureKey_; }
  size_t buffered() const { return buf_.size(); }
  // Drop any partial frame so a stray fragment can't misalign later frames.
  void clearBuffer() { buf_.clear(); }

  // Feed one raw notification. May produce a reply to write back (Reply),
  // signal the handshake finished (Ready), or produce decrypted app data (Data).
  Result feed(const uint8_t *data, size_t len, Bytes &out);

  // Encrypt an application frame (Modbus request) with the secure key.
  Bytes encrypt(const Bytes &plain);

 private:
  Result handleKex(const uint8_t *msg, size_t len, Bytes &out);
  Bytes aesEncrypt(const Bytes &plain, const uint8_t *key, size_t keyBits, const uint8_t *fixedIv);
  bool aesDecrypt(const uint8_t *frame, size_t len, Bytes &out);

  Bytes buf_;
  bool haveUnsecureKey_ = false;
  bool haveSecureKey_ = false;
  uint8_t unsecureIv_[16];
  uint8_t unsecureKey_[16];
  uint8_t secureKey_[32];
  uint8_t peerPub_[64];
  struct Ecc;
  Ecc *ecc_;
};
