/* certverify.c — X.509 certificate-chain verification for GregOS TLS.
   The real anti-MITM trust decision: verify every signature up the chain, bind
   the leaf to the requested hostname, enforce validity dates, and require the
   chain to terminate at a CA in the embedded root store. Freestanding, no libc.
   Verified offline against real chains (example.com, php.net) + adversarial
   cases in scratchpad/cert_test.c.                                            */

#include "../include/certverify.h"
#include "../include/x509.h"
#include "../include/crypto.h"
#include "../include/cert_roots.h"

typedef unsigned char      u8;
typedef unsigned long long u64;

#define MAX_CHAIN 10

/* ── small helpers ───────────────────────────────────────────────────────── */
static int span_eq(const u8* a, int al, const u8* b, int bl){
    if (al != bl) return 0;
    for (int i = 0; i < al; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

/* minimal DER TLV reader (subject-DN walking for CN extraction) */
static int rd(const u8** p, const u8* end, int* tag, const u8** v, int* vlen){
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
    *tag = t; *v = *p; *vlen = l; *p += l;
    return 1;
}

/* ── hostname matching (RFC 6125-style) ──────────────────────────────────── */
static int lc(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ci_eq(const u8* a, int al, const char* b, int bl){
    if (al != bl) return 0;
    for (int i = 0; i < al; ++i) if (lc(a[i]) != lc((u8)b[i])) return 0;
    return 1;
}
/* Match one dNSName/CN pattern against `host`. A wildcard is allowed only as the
   entire leftmost label ("*.example.com") and matches exactly one host label. */
static int dns_match(const u8* pat, int pl, const char* host, int hl){
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {
        int dot = -1;
        for (int i = 0; i < hl; ++i) if (host[i] == '.') { dot = i; break; }
        if (dot <= 0) return 0;                       /* need a label before '.' */
        /* remaining host from first dot (".example.com") must equal pat+1;
           equal lengths ⇒ wildcard consumed exactly one label */
        return ci_eq(pat + 1, pl - 1, host + dot, hl - dot);
    }
    return ci_eq(pat, pl, host, hl);
}

static const u8 OID_CN[] = { 0x55, 0x04, 0x03 };
/* extract the (last) commonName string from a raw subject Name DER */
static int subject_cn(const x509_cert* c, const u8** cn, int* cnl){
    const u8* p = c->subject; const u8* end = c->subject + c->subject_len;
    int tag; const u8* nv; int nl;
    if (!rd(&p, end, &tag, &nv, &nl) || tag != 0x30) return 0;   /* Name SEQUENCE */
    const u8* q = nv; const u8* qe = nv + nl; int found = 0;
    while (q < qe) {
        int t; const u8* rv; int rl;
        if (!rd(&q, qe, &t, &rv, &rl)) break;                    /* RDN = SET */
        const u8* a = rv; const u8* ae = rv + rl;
        while (a < ae) {
            int t2; const u8* av; int al;
            if (!rd(&a, ae, &t2, &av, &al)) break;                /* ATV SEQUENCE */
            const u8* b = av; const u8* be = av + al;
            int t3; const u8* oid; int ol;
            if (!rd(&b, be, &t3, &oid, &ol)) continue;            /* type OID */
            int t4; const u8* val; int vl;
            if (!rd(&b, be, &t4, &val, &vl)) continue;            /* value */
            if (ol == 3 && oid[0] == OID_CN[0] && oid[1] == OID_CN[1] && oid[2] == OID_CN[2]) {
                *cn = val; *cnl = vl; found = 1;                  /* keep last CN */
            }
        }
    }
    return found;
}

static int hostname_ok(const x509_cert* leaf, const char* host){
    int hl = 0; while (host[hl]) ++hl;
    if (hl == 0) return 0;
    if (leaf->san && leaf->san_len > 0) {
        const u8* p = leaf->san; const u8* end = p + leaf->san_len;
        int have_dns = 0;
        while (p < end) {
            int tag = *p++;
            if (p >= end) break;
            int l = *p++;
            if (l & 0x80) {
                int nb = l & 0x7f;
                if (nb < 1 || nb > 2 || p + nb > end) break;
                l = 0; for (int k = 0; k < nb; ++k) l = (l << 8) | *p++;
            }
            if (l < 0 || p + l > end) break;
            if (tag == 0x82) {                               /* GeneralName dNSName [2] */
                have_dns = 1;
                if (dns_match(p, l, host, hl)) return 1;
            }
            p += l;
        }
        if (have_dns) return 0;   /* SAN had dNSNames but none matched → reject (ignore CN) */
    }
    /* no SAN dNSName → fall back to subject CN */
    { const u8* cn = 0; int cnl = 0;
      if (subject_cn(leaf, &cn, &cnl) && dns_match(cn, cnl, host, hl)) return 1; }
    return 0;
}

/* ── trust anchoring against a root store ────────────────────────────────────
   Trust is ONLY ever established by a valid signature made with a store root's
   own key (never by a certificate merely claiming a root's identity). A cert
   used to sign another must be a CA (basicConstraints cA=TRUE, and keyUsage
   keyCertSign when the extension is present).                                 */
static int can_sign(const x509_cert* issuer){
    if (!issuer->is_ca) return 0;                          /* must be a CA          */
    if (issuer->has_key_usage && !issuer->key_cert_sign) return 0; /* keyCertSign   */
    /* EKU chaining: a CA constrained to other purposes (e.g. an S/MIME-only
       sub-CA, which needs no nameConstraints to be BR-compliant) must not be
       able to issue certificates usable for TLS server authentication.       */
    if (issuer->has_eku && !issuer->eku_server_auth) return 0;
    return 1;
}

/* Find a store root whose Subject == `issuer` DN and that validly signed
   `child`. The anchor must itself be a CA and be time-valid. Returns 1 on
   success. */
static int verify_by_root(const x509_cert* child, const u8* issuer, int issuer_len,
                          const cert_root* roots, int nroots, u64 now){
    for (int i = 0; i < nroots; ++i) {
        x509_cert r;
        if (!x509_parse(roots[i].der, roots[i].len, &r)) continue;
        if (!r.is_ca) continue;                            /* anchor must be a CA   */
        if (now) {                                         /* and time-valid        */
            if (r.not_before && now < r.not_before) continue;
            if (r.not_after  && now > r.not_after)  continue;
        }
        if (span_eq(issuer, issuer_len, r.subject, r.subject_len)
            && x509_verify_sig(child, &r))
            return 1;
    }
    return 0;
}

static int verify_impl(const u8* const* ders, const int* lens, int count,
                       const char* hostname, u64 now,
                       const cert_root* roots, int nroots){
    if (count < 1) return CERT_ERR_PARSE;
    if (count > MAX_CHAIN) return CERT_ERR_TOOMANY;

    x509_cert c[MAX_CHAIN];
    for (int i = 0; i < count; ++i)
        if (!x509_parse(ders[i], lens[i], &c[i])) return CERT_ERR_PARSE;

    /* 1. the leaf must be usable for TLS server authentication, and bound to
          the requested hostname */
    if (c[0].has_eku && !c[0].eku_server_auth) return CERT_ERR_PURPOSE;
    if (hostname && hostname[0])
        if (!hostname_ok(&c[0], hostname)) return CERT_ERR_HOSTNAME;

    /* 2. validity window on every cert */
    if (now) {
        for (int i = 0; i < count; ++i) {
            if (c[i].not_before && now < c[i].not_before) return CERT_ERR_NOT_YET_VALID;
            if (c[i].not_after  && now > c[i].not_after)  return CERT_ERR_EXPIRED;
        }
    }

    /* 3+4. walk from the leaf upward. At each cert, either its issuer is a
       trusted root (→ done), or the next cert in the chain is its issuer and
       must be a CA that validly signed it. Trust is only ever conferred by a
       signature from a store root's key, so copying a root's identity into a
       cert gains nothing, and a non-CA cert can never issue another.          */
    for (int i = 0; i < count; ++i) {
        if (verify_by_root(&c[i], c[i].issuer, c[i].issuer_len, roots, nroots, now))
            return CERT_OK;                                /* anchored at a root */
        if (i + 1 >= count) return CERT_ERR_UNTRUSTED;     /* ran off the top    */
        if (!span_eq(c[i].issuer, c[i].issuer_len, c[i+1].subject, c[i+1].subject_len))
            return CERT_ERR_CHAIN;
        if (!can_sign(&c[i+1])) return CERT_ERR_NOTCA;     /* issuer must be a CA */
        /* pathLenConstraint: c[i+1] issues c[i]; the intermediate CAs strictly
           below it toward the leaf are c[1..i] (i certs, c[0] is the leaf), so
           it may not be used if that exceeds its declared path length.        */
        if (c[i+1].has_path_len && i > c[i+1].path_len) return CERT_ERR_PATHLEN;
        if (!x509_verify_sig(&c[i], &c[i+1])) return CERT_ERR_BADSIG;
    }
    return CERT_ERR_UNTRUSTED;
}

/* ── public entry point ──────────────────────────────────────────────────── */
int cert_verify_chain(const u8* const* ders, const int* lens, int count,
                      const char* hostname, u64 now){
    return verify_impl(ders, lens, count, hostname, now, CERT_ROOTS, CERT_ROOTS_COUNT);
}

#ifdef CERTVERIFY_TEST
/* test hook: verify against a caller-supplied root store */
int cert_verify_chain_store(const u8* const* ders, const int* lens, int count,
                            const char* hostname, u64 now,
                            const cert_root* roots, int nroots){
    return verify_impl(ders, lens, count, hostname, now, roots, nroots);
}
#endif

/* DER INTEGER value → fixed w-byte big-endian (strip sign byte, left-pad). */
static int der_int_fixed(const u8* v, int vlen, u8* out, int w){
    while (vlen > 0 && v[0] == 0x00) { ++v; --vlen; }
    if (vlen > w || vlen == 0) return 0;
    for (int i = 0; i < w; ++i) out[i] = 0;
    for (int i = 0; i < vlen; ++i) out[w - vlen + i] = v[i];
    return 1;
}

int cert_verify_signature(const u8* leaf_der, int leaf_len,
                          int hash_alg, int sig_alg,
                          const u8* data, int dlen,
                          const u8* sig, int siglen){
    x509_cert leaf;
    if (!x509_parse(leaf_der, leaf_len, &leaf)) return 0;

    /* hash the signed data (TLS hash id: 4=SHA256, 5=SHA384) */
    u8 hash[48]; int hlen;
    if      (hash_alg == 4) { sha256(data, (unsigned)dlen, hash); hlen = 32; }
    else if (hash_alg == 5) { sha384(data, (unsigned)dlen, hash); hlen = 48; }
    else return 0;                                     /* unsupported hash → fail closed */

    if (sig_alg == 3) {                                /* ECDSA: DER SEQUENCE{r,s} */
        const u8* p = sig; const u8* end = sig + siglen;
        int tag; const u8* sv; int sl;
        if (!rd(&p, end, &tag, &sv, &sl) || tag != 0x30) return 0;
        const u8* q = sv; const u8* qe = sv + sl;
        int t2; const u8 *rv, *ssv; int rl, ssl;
        if (!rd(&q, qe, &t2, &rv, &rl)) return 0;
        if (!rd(&q, qe, &t2, &ssv, &ssl)) return 0;
        if (leaf.key_type == X509_KEY_EC_P256) {
            u8 r[32], s[32];
            if (!der_int_fixed(rv, rl, r, 32) || !der_int_fixed(ssv, ssl, s, 32)) return 0;
            return p256_ecdsa_verify(hash, r, s, leaf.ec_point + 1);   /* leftmost 256 bits */
        }
        if (leaf.key_type == X509_KEY_EC_P384) {
            u8 r[48], s[48];
            if (!der_int_fixed(rv, rl, r, 48) || !der_int_fixed(ssv, ssl, s, 48)) return 0;
            return p384_ecdsa_verify(hash, hlen, r, s, leaf.ec_point + 1);
        }
        return 0;                                      /* ECDSA sig but non-EC key */
    }
    if (sig_alg == 1) {                                /* RSA PKCS#1 v1.5 */
        if (leaf.key_type != X509_KEY_RSA) return 0;
        if (hlen != 32) return 0;                      /* only RSA-SHA256 supported here */
        return rsa_pkcs1_sha256_verify(leaf.rsa_n, leaf.rsa_n_len,
                                       leaf.rsa_e, leaf.rsa_e_len, sig, siglen, hash);
    }
    return 0;                                          /* unsupported sig alg */
}

const char* cert_strerror(int code){
    switch (code) {
        case CERT_OK:               return "ok";
        case CERT_ERR_PARSE:        return "certificate parse error";
        case CERT_ERR_HOSTNAME:     return "hostname does not match certificate";
        case CERT_ERR_NOT_YET_VALID:return "certificate not yet valid";
        case CERT_ERR_EXPIRED:      return "certificate expired";
        case CERT_ERR_CHAIN:        return "broken certificate chain";
        case CERT_ERR_BADSIG:       return "invalid certificate signature";
        case CERT_ERR_UNTRUSTED:    return "issuer not in trusted root store";
        case CERT_ERR_TOOMANY:      return "certificate chain too long";
        case CERT_ERR_NOTCA:        return "issuer is not a certificate authority";
        case CERT_ERR_PURPOSE:      return "certificate not valid for TLS server auth";
        case CERT_ERR_PATHLEN:      return "CA path length constraint exceeded";
        default:                    return "unknown certificate error";
    }
}
