#ifndef X509_H
#define X509_H

/* ── Minimal X.509 / ASN.1-DER parser for GregOS TLS certificate checking ────
   Parses one DER certificate, extracting the fields needed for chain
   verification. Pointers alias into the caller's DER buffer (no copies).    */

#ifdef __cplusplus
extern "C" {
#endif

enum { X509_SIG_UNKNOWN=0, X509_SIG_RSA_SHA256, X509_SIG_RSA_SHA384,
       X509_SIG_ECDSA_SHA256, X509_SIG_ECDSA_SHA384 };
enum { X509_KEY_UNKNOWN=0, X509_KEY_RSA, X509_KEY_EC_P256, X509_KEY_EC_P384 };

typedef struct {
    const unsigned char* tbs;   int tbs_len;   /* raw TBSCertificate (to hash) */
    int                  sig_alg;
    const unsigned char* sig;   int sig_len;    /* signatureValue contents      */
    int                  key_type;
    const unsigned char* rsa_n; int rsa_n_len;  /* modulus (leading 0x00 stripped) */
    const unsigned char* rsa_e; int rsa_e_len;
    const unsigned char* ec_point; int ec_point_len; /* 04||X||Y (65 or 97)     */
    const unsigned char* issuer;  int issuer_len;    /* raw DER Name            */
    const unsigned char* subject; int subject_len;
    unsigned long long   not_before, not_after;      /* YYYYMMDDHHMMSS          */
    const unsigned char* san; int san_len;           /* raw GeneralNames (or 0) */
    int                  is_ca;         /* basicConstraints cA = TRUE            */
    int                  has_key_usage; /* keyUsage extension present            */
    int                  key_cert_sign; /* keyUsage asserts keyCertSign          */
    int                  has_eku;       /* extendedKeyUsage present              */
    int                  eku_server_auth; /* EKU asserts serverAuth (or anyEKU)  */
    int                  has_path_len;  /* basicConstraints pathLenConstraint set */
    int                  path_len;      /* its value                             */
} x509_cert;

/* Parse a DER certificate. Returns 1 on success, 0 on malformed input. */
int x509_parse(const unsigned char* der, int len, x509_cert* c);

/* Verify that `child`'s signature is valid under `issuer`'s public key.
   Returns 1 if valid, 0 otherwise. */
int x509_verify_sig(const x509_cert* child, const x509_cert* issuer);

#ifdef __cplusplus
}
#endif

#endif /* X509_H */
