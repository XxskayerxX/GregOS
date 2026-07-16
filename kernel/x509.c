/* x509.c — minimal ASN.1-DER / X.509 parser for GregOS TLS cert verification.
   Extracts the fields needed to verify a certificate chain and checks one
   certificate's signature under its issuer's public key (RSA or ECDSA-P256).
   Freestanding, no libc. Verified offline against real cert chains + openssl. */

#include "../include/x509.h"
#include "../include/crypto.h"

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ── DER TLV reader ──────────────────────────────────────────────────────────
   Reads one element at *p (bounded by end): tag byte, value pointer+length,
   advancing *p past the whole element. `full`/`fulllen` give the element incl.
   its header (needed to hash the TBSCertificate). Returns 1 on success.     */
static int der(const u8** p, const u8* end, int* tag,
               const u8** full, int* fulllen, const u8** v, int* vlen){
    const u8* start = *p;
    if (*p >= end) return 0;
    int t = *(*p)++;
    if (*p >= end) return 0;
    int l = *(*p)++;
    if (l & 0x80) {
        int nb = l & 0x7f;
        if (nb < 1 || nb > 3) return 0;
        l = 0;
        for (int i = 0; i < nb; ++i) { if (*p >= end) return 0; l = (l << 8) | *(*p)++; }
    }
    if (l < 0 || *p + l > end) return 0;
    *tag = t; *v = *p; *vlen = l;
    *p += l;
    if (full) { *full = start; *fulllen = (int)(*p - start); }
    return 1;
}

/* Enter a constructed element's value as a new [p,end) cursor. */
#define CURSOR(vp, vl) const u8* p = (vp); const u8* end = (vp) + (vl)

static int oid_eq(const u8* v, int vlen, const u8* oid, int oidlen){
    if (vlen != oidlen) return 0;
    for (int i = 0; i < vlen; ++i) if (v[i] != oid[i]) return 0;
    return 1;
}
static const u8 OID_RSA[]       = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const u8 OID_ECPUB[]     = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const u8 OID_P256[]      = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const u8 OID_P384[]      = {0x2b,0x81,0x04,0x00,0x22};
static const u8 OID_ECDSA_256[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const u8 OID_ECDSA_384[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const u8 OID_RSA_256[]   = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const u8 OID_RSA_384[]   = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const u8 OID_SAN[]       = {0x55,0x1d,0x11};
static const u8 OID_BASIC[]     = {0x55,0x1d,0x13};   /* basicConstraints */
static const u8 OID_KEYUSAGE[]  = {0x55,0x1d,0x0f};   /* keyUsage         */
static const u8 OID_EKU[]       = {0x55,0x1d,0x25};   /* extendedKeyUsage */
static const u8 OID_KP_SERVER[] = {0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01}; /* id-kp-serverAuth */
static const u8 OID_EKU_ANY[]   = {0x55,0x1d,0x25,0x00};                     /* anyExtendedKeyUsage */

/* parse an AlgorithmIdentifier SEQUENCE value → sig_alg enum (0 if unknown) */
static int parse_sig_alg(const u8* v, int vlen){
    CURSOR(v, vlen); int tag, fl; const u8 *fu,*ov; int ol;
    if (!der(&p,end,&tag,&fu,&fl,&ov,&ol)) return 0;   /* OID */
    if (oid_eq(ov,ol,OID_ECDSA_256,sizeof OID_ECDSA_256)) return X509_SIG_ECDSA_SHA256;
    if (oid_eq(ov,ol,OID_ECDSA_384,sizeof OID_ECDSA_384)) return X509_SIG_ECDSA_SHA384;
    if (oid_eq(ov,ol,OID_RSA_256,sizeof OID_RSA_256))     return X509_SIG_RSA_SHA256;
    if (oid_eq(ov,ol,OID_RSA_384,sizeof OID_RSA_384))     return X509_SIG_RSA_SHA384;
    return X509_SIG_UNKNOWN;
}

/* parse SubjectPublicKeyInfo SEQUENCE value → key fields in c */
static int parse_spki(const u8* v, int vlen, x509_cert* c){
    CURSOR(v, vlen); int tag,fl; const u8 *fu,*algv,*keyv; int algl,keyl;
    if (!der(&p,end,&tag,&fu,&fl,&algv,&algl)) return 0;   /* algorithm SEQUENCE */
    if (!der(&p,end,&tag,&fu,&fl,&keyv,&keyl)) return 0;   /* subjectPublicKey BIT STRING */
    if (tag != 0x03 || keyl < 1) return 0;
    const u8* key = keyv + 1; int klen = keyl - 1;         /* skip unused-bits byte */
    /* algorithm: OID [+ params] */
    { CURSOR(algv, algl); int t2,f2; const u8 *o,*pr; int ol,pl;
      if (!der(&p,end,&t2,0,&f2,&o,&ol)) return 0;
      if (oid_eq(o,ol,OID_ECPUB,sizeof OID_ECPUB)) {
          if (!der(&p,end,&t2,0,&f2,&pr,&pl)) return 0;    /* namedCurve OID */
          if (oid_eq(pr,pl,OID_P256,sizeof OID_P256)) {
              if (klen != 65 || key[0] != 0x04) return 0;
              c->key_type = X509_KEY_EC_P256;
          } else if (oid_eq(pr,pl,OID_P384,sizeof OID_P384)) {
              if (klen != 97 || key[0] != 0x04) return 0;
              c->key_type = X509_KEY_EC_P384;
          } else return 0;
          c->ec_point = key; c->ec_point_len = klen;
          return 1;
      } else if (oid_eq(o,ol,OID_RSA,sizeof OID_RSA)) {
          CURSOR(key, klen);                               /* RSAPublicKey SEQUENCE */
          int t3,f3; const u8 *sv; int sl;
          if (!der(&p,end,&t3,0,&f3,&sv,&sl)) return 0;
          { CURSOR(sv, sl); int t4,f4; const u8 *nv,*ev; int nl,el;
            if (!der(&p,end,&t4,0,&f4,&nv,&nl)) return 0;  /* modulus */
            if (!der(&p,end,&t4,0,&f4,&ev,&el)) return 0;  /* exponent */
            while (nl > 0 && nv[0] == 0x00) { ++nv; --nl; }/* strip DER sign byte */
            while (el > 0 && ev[0] == 0x00) { ++ev; --el; }
            c->key_type = X509_KEY_RSA;
            c->rsa_n = nv; c->rsa_n_len = nl; c->rsa_e = ev; c->rsa_e_len = el;
            return 1; }
      }
      return 0; }
}

/* parse a Time (UTCTime/GeneralizedTime) → YYYYMMDDHHMMSS number */
static u64 parse_time(int tag, const u8* v, int vlen){
    int idx = 0; u64 year;
    if (tag == 0x17) {                                     /* UTCTime YYMMDD... */
        if (vlen < 12) return 0;
        int yy = (v[0]-'0')*10 + (v[1]-'0');
        year = (yy < 50) ? 2000 + yy : 1900 + yy; idx = 2;
    } else {                                               /* GeneralizedTime YYYYMMDD */
        if (vlen < 14) return 0;
        year = (u64)(v[0]-'0')*1000 + (v[1]-'0')*100 + (v[2]-'0')*10 + (v[3]-'0'); idx = 4;
    }
    u64 t = year;
    for (int k = 0; k < 5; ++k) t = t*100 + (u64)(v[idx+2*k]-'0')*10 + (v[idx+2*k+1]-'0');
    return t;   /* YYYYMMDDHHMMSS */
}

/* Scan the extensions [3] wrapper for SAN, basicConstraints, keyUsage and EKU.
   Returns 0 if the certificate must be REJECTED — in particular if it carries a
   critical extension we do not implement. RFC 5280 4.2 requires that; failing
   open there would silently void every constraint we don't parse (notably
   nameConstraints, which is always critical), so this is the load-bearing
   fail-closed check for a verifier that cannot do full path validation.     */
static int parse_exts(const u8* v, int vlen, x509_cert* c){
    CURSOR(v, vlen); int tag,fl; const u8 *fu,*seqv; int seql;
    if (!der(&p,end,&tag,&fu,&fl,&seqv,&seql)) return 1;   /* Extensions SEQUENCE */
    { CURSOR(seqv, seql);
      while (p < end) {
        int t2,f2; const u8 *extv; int extl;
        if (!der(&p,end,&t2,0,&f2,&extv,&extl)) return 1;   /* Extension SEQUENCE */
        { CURSOR(extv, extl); int t3,f3; const u8 *ov,*val; int ol,vl;
          int critical = 0;
          if (!der(&p,end,&t3,0,&f3,&ov,&ol)) continue;     /* extnID OID */
          if (!der(&p,end,&t3,0,&f3,&val,&vl)) continue;    /* critical BOOL or OCTET STRING */
          if (t3 == 0x01) {                                 /* critical BOOLEAN */
              critical = (vl >= 1 && val[0] != 0x00);
              if (!der(&p,end,&t3,0,&f3,&val,&vl)) continue;/* then the extnValue */
          }
          if (oid_eq(ov,ol,OID_EKU,sizeof OID_EKU)) {
              /* OCTET STRING → SEQUENCE OF KeyPurposeId */
              c->has_eku = 1;
              CURSOR(val, vl); int t4,f4; const u8 *sv; int sl;
              if (der(&p,end,&t4,0,&f4,&sv,&sl) && t4==0x30) {
                  CURSOR(sv, sl); int t5,f5; const u8 *kp; int kl;
                  while (p < end) {
                      if (!der(&p,end,&t5,0,&f5,&kp,&kl)) break;
                      if (oid_eq(kp,kl,OID_KP_SERVER,sizeof OID_KP_SERVER) ||
                          oid_eq(kp,kl,OID_EKU_ANY,sizeof OID_EKU_ANY))
                          c->eku_server_auth = 1;
                  }
              }
          } else if (!oid_eq(ov,ol,OID_SAN,sizeof OID_SAN) &&
                     !oid_eq(ov,ol,OID_BASIC,sizeof OID_BASIC) &&
                     !oid_eq(ov,ol,OID_KEYUSAGE,sizeof OID_KEYUSAGE)) {
              if (critical) return 0;      /* unrecognised critical → reject cert */
          }
          if (oid_eq(ov,ol,OID_SAN,sizeof OID_SAN)) {
              /* val is an OCTET STRING wrapping GeneralNames SEQUENCE */
              CURSOR(val, vl); int t4,f4; const u8 *gn; int gl;
              if (der(&p,end,&t4,0,&f4,&gn,&gl)) { c->san = gn; c->san_len = gl; }
          } else if (oid_eq(ov,ol,OID_BASIC,sizeof OID_BASIC)) {
              /* OCTET STRING → SEQUENCE { cA BOOLEAN DEFAULT FALSE,
                                          pathLenConstraint INTEGER OPTIONAL } */
              CURSOR(val, vl); int t4,f4; const u8 *sv; int sl;
              if (der(&p,end,&t4,0,&f4,&sv,&sl) && t4==0x30) {
                  CURSOR(sv, sl); int t5,f5; const u8 *ev; int el;
                  while (der(&p,end,&t5,0,&f5,&ev,&el)) {
                      if (t5==0x01 && el>=1 && ev[0]!=0x00) c->is_ca = 1; /* cA TRUE */
                      else if (t5==0x02) {                   /* pathLenConstraint */
                          int pv = 0;
                          for (int z=0; z<el && z<4; ++z) pv = (pv<<8) | ev[z];
                          c->has_path_len = 1; c->path_len = pv;
                      }
                  }
              }
          } else if (oid_eq(ov,ol,OID_KEYUSAGE,sizeof OID_KEYUSAGE)) {
              /* OCTET STRING → BIT STRING; keyCertSign is bit 5 (mask 0x04) */
              CURSOR(val, vl); int t4,f4; const u8 *bs; int bl;
              if (der(&p,end,&t4,0,&f4,&bs,&bl) && t4==0x03 && bl>=2) {
                  c->has_key_usage = 1;
                  if (bs[1] & 0x04) c->key_cert_sign = 1;    /* bs[0]=unused bits */
              }
          }
        }
      }
    }
    return 1;
}

int x509_parse(const u8* d, int len, x509_cert* c){
    for (u32 i=0;i<sizeof(*c);++i) ((u8*)c)[i]=0;
    const u8* p = d; const u8* end = d + len;
    int tag,fl; const u8 *fu,*certv; int certl;
    if (!der(&p,&*end,&tag,&fu,&fl,&certv,&certl) || tag != 0x30) return 0;  /* Certificate */
    { CURSOR(certv, certl);
      int t,f; const u8 *tbsfull,*tbsv,*algv,*sigv; int tbsfl,tbsl,algl,sigl;
      if (!der(&p,end,&t,&tbsfull,&tbsfl,&tbsv,&tbsl) || t!=0x30) return 0;   /* TBSCertificate */
      c->tbs = tbsfull; c->tbs_len = tbsfl;
      if (!der(&p,end,&t,0,&f,&algv,&algl)) return 0;                          /* signatureAlgorithm */
      c->sig_alg = parse_sig_alg(algv, algl);
      if (!der(&p,end,&t,0,&f,&sigv,&sigl) || t!=0x03 || sigl<1) return 0;     /* signatureValue */
      c->sig = sigv + 1; c->sig_len = sigl - 1;                                /* skip unused-bits */

      /* parse TBSCertificate */
      { CURSOR(tbsv, tbsl);
      int tt,ff; const u8 *ev; int el;
      if (!der(&p,end,&tt,0,&ff,&ev,&el)) return 0;
      if (tt == 0xA0) { if (!der(&p,end,&tt,0,&ff,&ev,&el)) return 0; }        /* skip version [0] */
      /* now at serialNumber (already read as ev/el) → skip; read next fields */
      if (!der(&p,end,&tt,0,&ff,&ev,&el)) return 0;                            /* signature AlgId */
      { const u8 *iv; int il;
        if (!der(&p,end,&tt,&fu,&fl,&iv,&il) || tt!=0x30) return 0;            /* issuer Name */
        c->issuer = fu; c->issuer_len = fl; }
      { const u8 *vv; int vl;
        if (!der(&p,end,&tt,0,&ff,&vv,&vl) || tt!=0x30) return 0;              /* validity SEQ */
        CURSOR(vv, vl); int t2,f2; const u8 *nb,*na; int nbl,nal;
        if (!der(&p,end,&t2,0,&f2,&nb,&nbl)) return 0;
        c->not_before = parse_time(t2,nb,nbl);
        if (!der(&p,end,&t2,0,&f2,&na,&nal)) return 0;
        c->not_after  = parse_time(t2,na,nal); }
      { const u8 *sv; int sl;
        if (!der(&p,end,&tt,&fu,&fl,&sv,&sl) || tt!=0x30) return 0;            /* subject Name */
        c->subject = fu; c->subject_len = fl; }
      { const u8 *kv; int kl;
        if (!der(&p,end,&tt,0,&ff,&kv,&kl) || tt!=0x30) return 0;              /* SPKI SEQ */
        if (!parse_spki(kv, kl, c)) return 0; }
      /* optional [1] issuerUID, [2] subjectUID, [3] extensions */
      while (p < end) {
        const u8 *xv; int xl;
        if (!der(&p,end,&tt,0,&ff,&xv,&xl)) break;
        if (tt == 0xA3) { if (!parse_exts(xv, xl, c)) return 0; break; }       /* extensions [3] */
      }
      }
    }
    return c->key_type != X509_KEY_UNKNOWN;
}

/* DER INTEGER value → fixed `w`-byte big-endian (strip sign byte, left-pad). */
static int int_to_fixed(const u8* v, int vlen, u8* out, int w){
    while (vlen > 0 && v[0] == 0x00) { ++v; --vlen; }
    if (vlen > w || vlen == 0) return 0;
    for (int i = 0; i < w; ++i) out[i] = 0;
    for (int i = 0; i < vlen; ++i) out[w - vlen + i] = v[i];
    return 1;
}

int x509_verify_sig(const x509_cert* child, const x509_cert* issuer){
    int alg = child->sig_alg;
    u8 hash[48]; int is384 = (alg == X509_SIG_ECDSA_SHA384 || alg == X509_SIG_RSA_SHA384);
    if (is384) sha384(child->tbs, (u32)child->tbs_len, hash);
    else       sha256(child->tbs, (u32)child->tbs_len, hash);

    if (alg == X509_SIG_ECDSA_SHA256 || alg == X509_SIG_ECDSA_SHA384) {
        int hlen = is384 ? 48 : 32;
        /* signatureValue is DER SEQUENCE { r INTEGER, s INTEGER } */
        CURSOR(child->sig, child->sig_len);
        int t,f; const u8 *sv; int sl;
        if (!der(&p,end,&t,0,&f,&sv,&sl) || t!=0x30) return 0;
        { CURSOR(sv, sl); int t2,f2; const u8 *rv,*svv; int rl,svl;
          if (!der(&p,end,&t2,0,&f2,&rv,&rl)) return 0;
          if (!der(&p,end,&t2,0,&f2,&svv,&svl)) return 0;
          if (issuer->key_type == X509_KEY_EC_P256) {
              u8 r[32], s[32];
              if (!int_to_fixed(rv,rl,r,32) || !int_to_fixed(svv,svl,s,32)) return 0;
              /* P-256 uses leftmost 256 bits of the digest → hash[0..31] */
              return p256_ecdsa_verify(hash, r, s, issuer->ec_point + 1);
          }
          if (issuer->key_type == X509_KEY_EC_P384) {
              u8 r[48], s[48];
              if (!int_to_fixed(rv,rl,r,48) || !int_to_fixed(svv,svl,s,48)) return 0;
              return p384_ecdsa_verify(hash, hlen, r, s, issuer->ec_point + 1);
          }
          return 0; }
    }
    if (alg == X509_SIG_RSA_SHA256) {
        if (issuer->key_type != X509_KEY_RSA) return 0;
        return rsa_pkcs1_sha256_verify(issuer->rsa_n, issuer->rsa_n_len,
                                       issuer->rsa_e, issuer->rsa_e_len,
                                       child->sig, child->sig_len, hash);
    }
    /* RSA-SHA384 not yet supported (needs a SHA-384 DigestInfo variant) */
    return 0;
}
