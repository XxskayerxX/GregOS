#ifndef TLS_H
#define TLS_H

/* ── GregOS TLS 1.2 client — HTTPS for the GregNet browser ───────────────────
   ECDHE (x25519) + AES-128-GCM-SHA256, over the existing TCP stack (net.h),
   using the crypto primitives in crypto.h. Certificate chains are parsed but
   NOT verified (no PKI) — this brings the modern (HTTPS) web within reach on
   bare metal; it is NOT a security guarantee.
   Implementation: kernel/tls.c.                                             */

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as http_get(), but for https:// URLs.
   Returns the HTTP status code (200, 404…), 0 on network error,
   -1 on unsupported/malformed URL, -2 on TLS handshake failure.            */
int https_get(const char* url, char** out_body, int* out_len, int max_len,
              char* final_url, char* content_type);

#ifdef __cplusplus
}
#endif

#endif /* TLS_H */
