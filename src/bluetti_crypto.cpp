#include "bluetti_crypto.h"

#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

namespace {

// Constants from the BLUETTI app (also present in app v3.1.5).
const uint8_t LOCAL_AES_KEY[16] = {0x45, 0x9F, 0xC5, 0x35, 0x80, 0x89, 0x41, 0xF1,
                                   0x70, 0x91, 0xE0, 0x99, 0x3E, 0xE3, 0xE9, 0x3D};
// Private key L1: signs our ephemeral public key.
const uint8_t PRIVATE_KEY_L1[32] = {0x4F, 0x19, 0xA1, 0x6E, 0x3E, 0x87, 0xBD, 0xD9, 0xBD, 0x24, 0xD3,
                                    0xE5, 0x49, 0x5B, 0x88, 0x04, 0x15, 0x11, 0x94, 0x3C, 0xBC, 0x8B,
                                    0x96, 0x9A, 0xDE, 0x96, 0x41, 0xD0, 0xF5, 0x6A, 0xF3, 0x37};
// Public key K2 (uncompressed point): verifies the device's public key.
const uint8_t PUBLIC_KEY_K2[65] = {
    0x04, 0xA7, 0x3A, 0xBF, 0x5D, 0x22, 0x32, 0xC8, 0xC1, 0xC7, 0x2E, 0x68, 0x30, 0x43, 0x43, 0xC2, 0x72,
    0x49, 0x5E, 0x3A, 0x8F, 0xD6, 0xF3, 0x0E, 0xA9, 0x6D, 0xE2, 0xF4, 0xB3, 0xCE, 0x60, 0xB2, 0x51,
    0xEE, 0x21, 0xAC, 0x66, 0x7C, 0xF8, 0xA7, 0x1E, 0x18, 0xB4, 0x6B, 0x66, 0x4E, 0xAE, 0xFF, 0xE3,
    0xC4, 0x89, 0xF2, 0x4F, 0x69, 0x5B, 0x64, 0x11, 0xDB, 0x7E, 0x22, 0xCC, 0xC8, 0x5A, 0x85, 0x94};

enum MsgType : uint8_t { CHALLENGE = 1, CHALLENGE_ACCEPTED = 3, PEER_PUBKEY = 4, PUBKEY_ACCEPTED = 6 };

int rng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

uint16_t sum16(const uint8_t *p, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return (uint16_t)s;
}

// "**" + body + big-endian byte sum of body
Bytes kexFrame(const Bytes &body) {
  Bytes f = {'*', '*'};
  f.insert(f.end(), body.begin(), body.end());
  uint16_t c = sum16(body.data(), body.size());
  f.push_back(c >> 8);
  f.push_back(c & 0xFF);
  return f;
}

bool isKex(const uint8_t *p, size_t n) { return n >= 6 && p[0] == '*' && p[1] == '*'; }

}  // namespace

struct BluettiCrypto::Ecc {
  mbedtls_ecp_group grp;
  mbedtls_mpi myD;
  mbedtls_ecp_point myQ;
  Ecc() {
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&myD);
    mbedtls_ecp_point_init(&myQ);
    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
  }
  ~Ecc() {
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&myD);
    mbedtls_ecp_point_free(&myQ);
  }
};

BluettiCrypto::BluettiCrypto() : ecc_(new Ecc()) {}
BluettiCrypto::~BluettiCrypto() { delete ecc_; }

void BluettiCrypto::reset() {
  buf_.clear();
  haveUnsecureKey_ = false;
  haveSecureKey_ = false;
}

Bytes BluettiCrypto::aesEncrypt(const Bytes &plain, const uint8_t *key, size_t keyBits,
                                const uint8_t *fixedIv) {
  Bytes out = {(uint8_t)(plain.size() >> 8), (uint8_t)(plain.size() & 0xFF)};
  uint8_t iv[16];
  if (fixedIv) {
    memcpy(iv, fixedIv, 16);
  } else {
    uint8_t seed[4];
    esp_fill_random(seed, 4);
    mbedtls_md5_ret(seed, 4, iv);
    out.insert(out.end(), seed, seed + 4);
  }
  Bytes padded = plain;
  padded.resize((plain.size() + 15) / 16 * 16, 0);
  Bytes enc(padded.size());
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, keyBits);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded.size(), iv, padded.data(), enc.data());
  mbedtls_aes_free(&aes);
  out.insert(out.end(), enc.begin(), enc.end());
  return out;
}

bool BluettiCrypto::aesDecrypt(const uint8_t *frame, size_t len, Bytes &out) {
  size_t dataLen = (frame[0] << 8) | frame[1];
  uint8_t iv[16];
  const uint8_t *key;
  size_t keyBits, hdr;
  if (haveSecureKey_) {
    mbedtls_md5_ret(frame + 2, 4, iv);
    key = secureKey_;
    keyBits = 256;
    hdr = 6;
  } else {
    memcpy(iv, unsecureIv_, 16);
    key = unsecureKey_;
    keyBits = 128;
    hdr = 2;
  }
  size_t encLen = len - hdr;
  if (encLen % 16 || dataLen > encLen) return false;
  out.resize(encLen);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, keyBits);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, encLen, iv, frame + hdr, out.data());
  mbedtls_aes_free(&aes);
  out.resize(dataLen);
  return true;
}

Bytes BluettiCrypto::encrypt(const Bytes &plain) { return aesEncrypt(plain, secureKey_, 256, nullptr); }

BluettiCrypto::Result BluettiCrypto::feed(const uint8_t *data, size_t len, Bytes &out) {
  // Plaintext key-exchange frames arrive whole.
  if (buf_.empty() && isKex(data, len)) return handleKex(data, len, out);

  if (!haveUnsecureKey_) {
    log_w("encrypted data before challenge, ignoring");
    return Result::None;
  }

  buf_.insert(buf_.end(), data, data + len);
  if (buf_.size() < 2) return Result::None;
  size_t dataLen = (buf_[0] << 8) | buf_[1];
  size_t expected = (haveSecureKey_ ? 6 : 2) + (dataLen + 15) / 16 * 16;
  if (buf_.size() < expected) return Result::None;  // wait for more fragments

  Bytes plain;
  bool ok = aesDecrypt(buf_.data(), expected, plain);
  buf_.erase(buf_.begin(), buf_.begin() + expected);
  if (!ok) {
    buf_.clear();
    log_e("decrypt failed");
    return Result::Error;
  }
  if (isKex(plain.data(), plain.size())) return handleKex(plain.data(), plain.size(), out);
  out = std::move(plain);
  return Result::Data;
}

BluettiCrypto::Result BluettiCrypto::handleKex(const uint8_t *msg, size_t len, Bytes &out) {
  const uint8_t *body = msg + 2;
  size_t bodyLen = len - 4;
  uint16_t cks = (msg[len - 2] << 8) | msg[len - 1];
  if (sum16(body, bodyLen) != cks) log_w("kex checksum mismatch");
  uint8_t type = body[0];
  const uint8_t *d = body + 2;
  size_t dLen = bodyLen - 2;

  switch (type) {
    case CHALLENGE: {
      if (dLen != 4) return Result::Error;
      uint8_t rev[4] = {d[3], d[2], d[1], d[0]};
      mbedtls_md5_ret(rev, 4, unsecureIv_);
      for (int i = 0; i < 16; i++) unsecureKey_[i] = unsecureIv_[i] ^ LOCAL_AES_KEY[i];
      haveUnsecureKey_ = true;
      haveSecureKey_ = false;
      Bytes b = {0x02, 0x04};
      b.insert(b.end(), unsecureIv_ + 8, unsecureIv_ + 12);
      out = kexFrame(b);
      log_i("challenge received, replying");
      return Result::Reply;
    }
    case CHALLENGE_ACCEPTED:
      log_i("challenge accepted");
      return Result::None;

    case PEER_PUBKEY: {
      if (dLen != 128) return Result::Error;
      memcpy(peerPub_, d, 64);

      // Verify device pubkey signature: ECDSA-SHA256(pub || unsecureIv) with K2.
      {
        uint8_t hash[32];
        uint8_t signedData[80];
        memcpy(signedData, d, 64);
        memcpy(signedData + 64, unsecureIv_, 16);
        mbedtls_sha256_ret(signedData, 80, hash, 0);
        mbedtls_ecp_point k2;
        mbedtls_mpi r, s;
        mbedtls_ecp_point_init(&k2);
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);
        mbedtls_ecp_point_read_binary(&ecc_->grp, &k2, PUBLIC_KEY_K2, 65);
        mbedtls_mpi_read_binary(&r, d + 64, 32);
        mbedtls_mpi_read_binary(&s, d + 96, 32);
        int rc = mbedtls_ecdsa_verify(&ecc_->grp, hash, 32, &k2, &r, &s);
        mbedtls_ecp_point_free(&k2);
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        if (rc != 0) {
          log_e("device pubkey signature invalid (%d)", rc);
          return Result::Error;
        }
      }

      // Ephemeral keypair.
      mbedtls_ecp_gen_keypair(&ecc_->grp, &ecc_->myD, &ecc_->myQ, rng, nullptr);
      uint8_t myPub[65];
      size_t olen;
      mbedtls_ecp_point_write_binary(&ecc_->grp, &ecc_->myQ, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, myPub,
                                     sizeof(myPub));

      // Sign (myPub || unsecureIv) with L1, raw r||s.
      uint8_t sig[64];
      {
        uint8_t hash[32];
        uint8_t toSign[80];
        memcpy(toSign, myPub + 1, 64);
        memcpy(toSign + 64, unsecureIv_, 16);
        mbedtls_sha256_ret(toSign, 80, hash, 0);
        mbedtls_mpi l1, r, s;
        mbedtls_mpi_init(&l1);
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);
        mbedtls_mpi_read_binary(&l1, PRIVATE_KEY_L1, 32);
        mbedtls_ecdsa_sign(&ecc_->grp, &r, &s, &l1, hash, 32, rng, nullptr);
        mbedtls_mpi_write_binary(&r, sig, 32);
        mbedtls_mpi_write_binary(&s, sig + 32, 32);
        mbedtls_mpi_free(&l1);
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
      }

      Bytes b = {0x05, 0x80};
      b.insert(b.end(), myPub + 1, myPub + 65);
      b.insert(b.end(), sig, sig + 64);
      out = aesEncrypt(kexFrame(b), unsecureKey_, 128, unsecureIv_);
      log_i("peer pubkey verified, sent ours");
      return Result::Reply;
    }
    case PUBKEY_ACCEPTED: {
      if (dLen != 1 || d[0] != 0) {
        log_e("pubkey rejected");
        return Result::Error;
      }
      uint8_t peer[65] = {0x04};
      memcpy(peer + 1, peerPub_, 64);
      mbedtls_ecp_point q;
      mbedtls_mpi z;
      mbedtls_ecp_point_init(&q);
      mbedtls_mpi_init(&z);
      mbedtls_ecp_point_read_binary(&ecc_->grp, &q, peer, 65);
      int rc = mbedtls_ecdh_compute_shared(&ecc_->grp, &z, &q, &ecc_->myD, rng, nullptr);
      mbedtls_mpi_write_binary(&z, secureKey_, 32);
      mbedtls_ecp_point_free(&q);
      mbedtls_mpi_free(&z);
      if (rc != 0) return Result::Error;
      haveSecureKey_ = true;
      log_i("secure session established");
      return Result::Ready;
    }
    default:
      log_w("unknown kex type %d", type);
      return Result::None;
  }
}
