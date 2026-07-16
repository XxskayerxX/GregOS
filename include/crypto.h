#ifndef CRYPTO_H
#define CRYPTO_H

/* ── GregOS crypto primitives — freestanding, no libc, no libgcc ─────────────
   Everything needed for a TLS 1.2 ECDHE handshake with AES-128-GCM:
     · SHA-256 (+ streaming ctx for the handshake transcript)
     · HMAC-SHA256
     · TLS 1.2 PRF (P_SHA256)
     · AES-128-GCM AEAD (encrypt + authenticated decrypt)
     · X25519 (RFC 7748 Montgomery ladder, TweetNaCl-derived)
   Implementation: kernel/crypto.c. Verified against known-answer test vectors
   on the host (scratchpad/crypto_test.c) before being wired into the stack. */

#ifdef __cplusplus
extern "C" {
#endif

/* ── SHA-256 ─────────────────────────────────────────────────────────────── */
typedef struct {
    unsigned int       h[8];
    unsigned long long bitlen;
    unsigned char      buf[64];
    unsigned int       idx;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const unsigned char* data, unsigned int len);
void sha256_final(sha256_ctx* c, unsigned char out[32]);
void sha256(const unsigned char* data, unsigned int len, unsigned char out[32]);

/* ── SHA-384 (one-shot; for certificate signatures using SHA-384) ─────────── */
void sha384(const unsigned char* data, unsigned int len, unsigned char out[48]);

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────────── */
void hmac_sha256(const unsigned char* key, unsigned int klen,
                 const unsigned char* msg, unsigned int mlen,
                 unsigned char out[32]);

/* ── TLS 1.2 PRF (P_SHA256) ──────────────────────────────────────────────────
   out = PRF(secret, label, seed) where the label is a NUL-terminated ASCII
   string; the PRF seed fed to P_hash is (label-bytes || seed).             */
void tls12_prf(const unsigned char* secret, unsigned int slen,
               const char* label,
               const unsigned char* seed, unsigned int seedlen,
               unsigned char* out, unsigned int outlen);

/* ── AES-128-GCM ─────────────────────────────────────────────────────────────
   iv is the 12-byte nonce. encrypt: ct[ptlen] + tag[16] produced.
   decrypt: returns 1 on tag match (pt[ctlen] filled), 0 on auth failure.   */
void aes128_gcm_encrypt(const unsigned char key[16], const unsigned char iv[12],
                        const unsigned char* aad, unsigned int aadlen,
                        const unsigned char* pt, unsigned int ptlen,
                        unsigned char* ct, unsigned char tag[16]);
int  aes128_gcm_decrypt(const unsigned char key[16], const unsigned char iv[12],
                        const unsigned char* aad, unsigned int aadlen,
                        const unsigned char* ct, unsigned int ctlen,
                        const unsigned char tag[16], unsigned char* pt);

/* ── X25519 (RFC 7748) ───────────────────────────────────────────────────────
   out = scalar · point (32-byte little-endian). _base uses u=9 (get pubkey). */
void x25519(unsigned char out[32], const unsigned char scalar[32],
            const unsigned char point[32]);
void x25519_base(unsigned char out[32], const unsigned char scalar[32]);

/* ── NIST P-256 / secp256r1 ECDHE ─────────────────────────────────────────────
   Points are 64 bytes: X(32) || Y(32) big-endian (TLS uncompressed, sans 0x04).
   Private scalar is 32 bytes big-endian.                                     */
void p256_scalarmult_base(unsigned char out_pub[64], const unsigned char scalar[32]);
int  p256_ecdh(unsigned char out_x[32], const unsigned char scalar[32],
               const unsigned char peer[64]);   /* 1 = ok, 0 = invalid peer point */
/* Verify an ECDSA-P256 signature (r,s) over a 32-byte hash against a 64-byte
   public key (X||Y big-endian). Returns 1 if valid, 0 otherwise.            */
int  p256_ecdsa_verify(const unsigned char hash[32], const unsigned char r[32],
                       const unsigned char s[32], const unsigned char pub[64]);

/* ── NIST P-384 / secp384r1 ECDSA verification ────────────────────────────────
   Verify an ECDSA-P384 signature (r,s big-endian 48 bytes) over `hash`
   (`hashlen` = 32 for SHA-256 or 48 for SHA-384; leftmost 384 bits are used)
   against a 96-byte public key (X||Y big-endian). Returns 1 if valid.        */
int  p384_ecdsa_verify(const unsigned char* hash, int hashlen,
                       const unsigned char r[48], const unsigned char s[48],
                       const unsigned char pub[96]);

/* ── RSA PKCS#1 v1.5 signature verification (SHA-256) ─────────────────────────
   Modulus n, exponent e, signature sig — all big-endian. hash = SHA-256 of the
   signed data. Returns 1 if valid. Supports moduli up to 4096-bit.           */
int  rsa_pkcs1_sha256_verify(const unsigned char* n, int nlen,
                             const unsigned char* e, int elen,
                             const unsigned char* sig, int siglen,
                             const unsigned char hash[32]);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_H */
