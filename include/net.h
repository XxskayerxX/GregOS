#ifndef NET_H
#define NET_H

/* ── GregOS network stack — public C API ─────────────────────────────────
   Implementation: kernel/net.c  (PCI scan, RTL8139 driver, Ethernet,
   ARP, IPv4, ICMP/UDP/TCP, DNS, HTTP).
   The stack is POLLED, not interrupt-driven: net_poll() drains the NIC RX
   ring and feeds the protocol handlers. All blocking helpers (ping, DNS,
   TCP, HTTP) pump net_poll() internally with jiffies-based timeouts, so
   they are safe to call from the main loop / window event handlers.

   Default configuration matches QEMU user networking (slirp):
     IP 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3.

   All IPv4 addresses in this API are HOST byte order (0x0A00020F).       */

#ifdef __cplusplus
extern "C" {
#endif

/* ── lifecycle ─────────────────────────────────────────────────────────── */
int  net_init(void);              /* scan PCI for RTL8139; 1 = NIC up      */
int  net_ready(void);             /* 1 if net_init() succeeded             */
void net_poll(void);              /* drain RX ring; call often when idle   */

/* ── configuration / info ──────────────────────────────────────────────── */
unsigned int net_local_ip(void);  /* host order, 0 if not ready            */
unsigned int net_gateway_ip(void);
unsigned int net_dns_ip(void);
void         net_get_mac(unsigned char mac[6]);

/* Runtime counters (for ifconfig / system monitor) */
unsigned int net_rx_packets(void);
unsigned int net_tx_packets(void);
unsigned int net_rx_bytes(void);
unsigned int net_tx_bytes(void);

/* ── helpers ───────────────────────────────────────────────────────────── */
unsigned int net_parse_ip(const char* s);        /* "a.b.c.d" → host u32, 0=err */
void         net_format_ip(unsigned int ip, char* buf16);

/* ── ICMP ──────────────────────────────────────────────────────────────── */
/* Sends an echo request; returns RTT in ms, or -1 on timeout/error.       */
int net_ping(unsigned int ip, int timeout_ms);

/* ── DNS ───────────────────────────────────────────────────────────────── */
/* Resolve an A record via the configured DNS server (UDP/53, 3 retries).
   Accepts dotted-quad input too. Returns host-order IPv4, 0 on failure.   */
unsigned int net_dns_resolve(const char* hostname);

/* ── TCP client ────────────────────────────────────────────────────────── */
/* Small fixed pool of client sockets. All calls block (pumping net_poll). */
int  tcp_connect(unsigned int ip, unsigned short port, int timeout_ms);
                                  /* → sock id ≥ 0, or -1                  */
int  tcp_send(int sock, const void* data, int len);   /* len sent, or -1  */
int  tcp_recv(int sock, void* buf, int maxlen, int timeout_ms);
                                  /* → n bytes; 0 = peer closed; -1 = t/o */
int  tcp_is_open(int sock);       /* 1 while ESTABLISHED / data pending    */
void tcp_close(int sock);

/* ── HTTP/1.0 client ───────────────────────────────────────────────────── */
/* GET an http:// URL (no TLS). Follows up to 5 http→http redirects.
   *out_body: kmalloc'ed buffer (caller must kfree), NUL-terminated.
   *out_len:  body length in bytes (capped at max_len).
   final_url: char[512] receiving the post-redirect URL (or NULL).
   content_type: char[64] receiving the Content-Type value (or NULL).
   Returns the HTTP status code (200, 404, …), 0 on network error,
   -1 on unsupported/malformed URL (e.g. https://).                        */
int http_get(const char* url, char** out_body, int* out_len, int max_len,
             char* final_url, char* content_type);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NET_H */
