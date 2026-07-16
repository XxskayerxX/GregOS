#ifndef CERTVERIFY_H
#define CERTVERIFY_H

/* ── Certificate-chain verification for GregOS TLS ───────────────────────────
   Ties the X.509 parser + signature primitives together into the real trust
   decision: build the chain leaf→…→root, verify every signature, match the
   hostname against the leaf's SAN/CN, check validity dates, and require the
   chain to reach a CA in the embedded root store (include/cert_roots.h).      */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CERT_OK = 0,
    CERT_ERR_PARSE,          /* a certificate failed to parse                  */
    CERT_ERR_HOSTNAME,       /* leaf does not match the requested hostname     */
    CERT_ERR_NOT_YET_VALID,  /* now < notBefore                                */
    CERT_ERR_EXPIRED,        /* now > notAfter                                 */
    CERT_ERR_CHAIN,          /* issuer/subject name chaining broken            */
    CERT_ERR_BADSIG,         /* a link signature did not verify                */
    CERT_ERR_UNTRUSTED,      /* chain does not reach a trusted root            */
    CERT_ERR_TOOMANY,        /* chain longer than we accept                    */
    CERT_ERR_NOTCA,          /* an issuer is not a CA (basicConstraints/keyUsage) */
    CERT_ERR_PURPOSE,        /* certificate not valid for TLS server auth (EKU)   */
    CERT_ERR_PATHLEN,        /* basicConstraints pathLenConstraint exceeded       */
};

/* Verify a server certificate chain. `ders`/`lens` are `count` DER-encoded
   certificates, leaf first (order as sent in the TLS Certificate message).
   `hostname` is the SNI name (NUL-terminated; pass 0/empty to skip the name
   check). `now` is the current time as a YYYYMMDDHHMMSS integer (pass 0 to skip
   the date check). Returns CERT_OK (0) on success or a CERT_ERR_* code.       */
int cert_verify_chain(const unsigned char* const* ders, const int* lens, int count,
                      const char* hostname, unsigned long long now);

/* Human-readable message for a CERT_* code. */
const char* cert_strerror(int code);

/* Verify an external signature (e.g. a TLS 1.2 ServerKeyExchange) made with the
   leaf certificate's private key — this binds the ephemeral ECDHE key to the
   authenticated certificate (without it, chain trust does not stop an active
   MITM). `leaf_der` is the leaf certificate DER. `hash_alg`/`sig_alg` are the
   TLS SignatureAndHashAlgorithm bytes (hash: 4=SHA256, 5=SHA384; sig: 1=RSA,
   3=ECDSA). `data`/`dlen` is the signed data; `sig`/`siglen` the signature.
   Returns 1 if valid, 0 otherwise (including unsupported scheme → fail closed). */
int cert_verify_signature(const unsigned char* leaf_der, int leaf_len,
                          int hash_alg, int sig_alg,
                          const unsigned char* data, int dlen,
                          const unsigned char* sig, int siglen);

#ifdef __cplusplus
}
#endif

#endif /* CERTVERIFY_H */
