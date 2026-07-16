/* ── GregOS network stack ────────────────────────────────────────────────
   PCI scan → RTL8139 NIC driver → Ethernet → ARP → IPv4 → ICMP / UDP / TCP
   → DNS → HTTP/1.0 client.  RX is IRQ-driven (Phase 9.1): irq_net_handler
   drains the hardware ring into an SPSC queue; net_poll() pops it from
   mainline, and the blocking helpers hlt between packets.

   Freestanding C, no libc.  Matches QEMU user networking (slirp):
     IP 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3.

   This file is intentionally self-contained: all byte-order helpers, packet
   builders and the protocol state machines live here.                       */

#include "../include/net.h"
#include "../include/ports.h"

/* ── kernel imports ──────────────────────────────────────────────────────── */
extern void*  kmalloc(unsigned int size);
extern void   kfree(void* p);
extern void*  memcpy(void* dst, const void* src, unsigned int n);
extern void   memset(void* ptr, int val, int size);
extern int    memcmp(const void* a, const void* b, unsigned int n);
extern int    strlen(const char* s);
extern volatile unsigned long jiffies;    /* 100 Hz */

/* Diagnostic logging routes to the active TTY (visible in terminal window). */
extern void term_print(const char* s);
extern void term_print_int(int n);
extern void kernel_net_irq_install(unsigned int vector);   /* kernel.c */

/* ── types ───────────────────────────────────────────────────────────────── */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ── byte order (x86 is little-endian; network is big-endian) ────────────── */
static inline u16 bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u32 bswap32(u32 v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) |
           ((v << 8) & 0x00FF0000u) | (v << 24);
}
#define htons(x) bswap16((u16)(x))
#define ntohs(x) bswap16((u16)(x))
#define htonl(x) bswap32((u32)(x))
#define ntohl(x) bswap32((u32)(x))

/* Read/write big-endian fields from a byte buffer (unaligned-safe). */
static inline u16 rd16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 rd32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline void wr16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* ── configuration ───────────────────────────────────────────────────────── */
/* Runtime network config — defaults match QEMU slirp; DHCP may overwrite. */
static u32 s_ip  = 0x0A00020Fu;   /* 10.0.2.15 */
static u32 s_gw  = 0x0A000202u;   /* 10.0.2.2  */
static u32 s_dns = 0x0A000203u;   /* 10.0.2.3  */
static int s_dhcp_active = 0;     /* accept any UDP:68 while negotiating */
/* Keep the historical macro spellings so the rest of net.c is untouched. */
#define IP_LOCAL   s_ip
#define IP_GATEWAY s_gw
#define IP_DNS     s_dns
#define IP_NETMASK 0xFFFFFF00u   /* /24 */

#define ETH_ARP  0x0806
#define ETH_IPV4 0x0800
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* ── driver state ────────────────────────────────────────────────────────── */
static int  s_ready = 0;
static u16  s_iobase = 0;
static u8   s_mac[6];
static u8   s_gw_mac[6];
static int  s_gw_mac_known = 0;

static u8*  s_rxbuf = 0;          /* 8192 + 16 + 1536 ring                     */
static u32  s_rx_offset = 0;      /* CAPR-tracked read cursor into ring        */
static u8*  s_txbuf[4] = {0,0,0,0};
static int  s_tx_cur = 0;

static u32  s_rx_packets = 0, s_tx_packets = 0, s_rx_bytes = 0, s_tx_bytes = 0;

/* ── RTL8139 registers (offsets from I/O base) ───────────────────────────── */
#define RTL_IDR0     0x00   /* MAC (6 bytes)          */
#define RTL_TSD0     0x10   /* transmit status 0..3   */
#define RTL_TSAD0    0x20   /* transmit start addr 0..3 */
#define RTL_RBSTART  0x30   /* RX buffer phys addr    */
#define RTL_CMD      0x37   /* command register       */
#define RTL_CAPR     0x38   /* current addr of packet read */
#define RTL_CBR      0x3A   /* current buffer addr    */
#define RTL_IMR      0x3C   /* interrupt mask         */
#define RTL_ISR      0x3E   /* interrupt status       */
#define RTL_TCR      0x40   /* transmit config        */
#define RTL_RCR      0x44   /* receive config         */
#define RTL_CONFIG1  0x52

#define CMD_RST      0x10
#define CMD_RE       0x08   /* receiver enable        */
#define CMD_TE       0x04   /* transmitter enable     */
#define CMD_BUFE     0x01   /* RX buffer empty        */

/* RX packet header status bits (first u16 of each ring entry) */
#define RX_ROK       0x0001

/* ── PCI configuration space ─────────────────────────────────────────────── */
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static u32 pci_read32(u8 bus, u8 slot, u8 func, u8 off) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
               ((u32)func << 8) | (off & 0xFC);
    port_dword_out(PCI_ADDR, addr);
    return port_dword_in(PCI_DATA);
}
static void pci_write32(u8 bus, u8 slot, u8 func, u8 off, u32 val) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
               ((u32)func << 8) | (off & 0xFC);
    port_dword_out(PCI_ADDR, addr);
    port_dword_out(PCI_DATA, val);
}

/* Find RTL8139 (vendor 0x10EC, device 0x8139). Returns 1 and fills bus/slot. */
static int pci_find_rtl8139(u8* out_bus, u8* out_slot) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            u32 id = pci_read32((u8)bus, (u8)slot, 0, 0x00);
            u16 vendor = (u16)(id & 0xFFFF);
            if (vendor == 0xFFFF) continue;
            u16 device = (u16)(id >> 16);
            if (vendor == 0x10EC && device == 0x8139) {
                *out_bus = (u8)bus; *out_slot = (u8)slot;
                return 1;
            }
        }
    }
    return 0;
}

/* ── small utils ─────────────────────────────────────────────────────────── */
static void mac_copy(u8* dst, const u8* src) { for (int i = 0; i < 6; i++) dst[i] = src[i]; }

/* Internet checksum (RFC 1071) over a byte range. */
static u16 inet_cksum(const u8* data, int len) {
    u32 sum = 0;
    int i = 0;
    for (; i + 1 < len; i += 2) sum += (u32)((data[i] << 8) | data[i + 1]);
    if (i < len) sum += (u32)(data[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/* ── transmit a raw Ethernet frame ───────────────────────────────────────── */
static void eth_send(const u8* dst_mac, u16 ethertype, const u8* payload, int plen) {
    if (!s_ready) return;
    int total = 14 + plen;
    if (total < 60) total = 60;                 /* pad to minimum frame        */
    if (total > 1514) total = 1514;
    u8* f = s_txbuf[s_tx_cur];
    memset(f, 0, total);
    mac_copy(f, dst_mac);
    mac_copy(f + 6, s_mac);
    wr16(f + 12, ethertype);
    if (plen > 0) memcpy(f + 14, payload, (unsigned)(plen > 1500 ? 1500 : plen));

    port_dword_out(s_iobase + RTL_TSAD0 + s_tx_cur * 4, (u32)(unsigned long)f);
    /* TSD: writing the size (bits 0..12) with OWN=0 kicks the transmit.       */
    port_dword_out(s_iobase + RTL_TSD0 + s_tx_cur * 4, (u32)total);

    /* Wait (bounded) for TOK, so back-to-back sends reuse descriptors safely. */
    for (int spin = 0; spin < 100000; spin++) {
        u32 st = port_dword_in(s_iobase + RTL_TSD0 + s_tx_cur * 4);
        if (st & 0x8000) break;                 /* TOK — transmit OK           */
    }
    s_tx_cur = (s_tx_cur + 1) & 3;
    s_tx_packets++; s_tx_bytes += (u32)total;
}

/* ── ARP ─────────────────────────────────────────────────────────────────── */
/* ARP cache: only the gateway matters for slirp (everything is off-subnet).  */
static void arp_send(u16 op, const u8* target_mac, u32 target_ip) {
    u8 p[28];
    wr16(p + 0, 1);              /* HTYPE ethernet   */
    wr16(p + 2, ETH_IPV4);      /* PTYPE IPv4       */
    p[4] = 6; p[5] = 4;          /* HLEN / PLEN      */
    wr16(p + 6, op);            /* opcode           */
    mac_copy(p + 8, s_mac);      /* sender MAC       */
    wr32(p + 14, IP_LOCAL);      /* sender IP        */
    mac_copy(p + 18, target_mac);/* target MAC       */
    wr32(p + 24, target_ip);     /* target IP        */
    static const u8 bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_send(op == 1 ? bcast : target_mac, ETH_ARP, p, 28);
}

static void arp_handle(const u8* p, int len) {
    if (len < 28) return;
    u16 op = rd16(p + 6);
    u32 sender_ip = rd32(p + 14);
    u32 target_ip = rd32(p + 24);
    if (op == 1 && target_ip == IP_LOCAL) {          /* request for us → reply */
        arp_send(2, p + 8, sender_ip);
    } else if (op == 2 && sender_ip == IP_GATEWAY) { /* reply from gateway     */
        mac_copy(s_gw_mac, p + 8);
        s_gw_mac_known = 1;
    }
}

/* Resolve the gateway MAC (blocking, bounded). Returns 1 on success. */
static int arp_resolve_gateway(void) {
    if (s_gw_mac_known) return 1;
    for (int tries = 0; tries < 4 && !s_gw_mac_known; tries++) {
        static const u8 zero[6] = {0,0,0,0,0,0};
        arp_send(1, zero, IP_GATEWAY);
        unsigned long deadline = jiffies + 50;       /* 500 ms                 */
        while (jiffies < deadline && !s_gw_mac_known) { net_poll(); __asm__ volatile("hlt"); }
    }
    return s_gw_mac_known;
}

/* ── IPv4 send ───────────────────────────────────────────────────────────── */
static u16 s_ip_id = 1;

static void ipv4_send(u32 dst_ip, u8 proto, const u8* payload, int plen) {
    if (!arp_resolve_gateway()) return;
    u8 pkt[1500];
    int hl = 20;
    if (plen > (1500 - hl)) plen = 1500 - hl;
    memset(pkt, 0, hl);
    pkt[0] = 0x45;                       /* version 4, IHL 5     */
    pkt[1] = 0;                          /* DSCP/ECN             */
    wr16(pkt + 2, (u16)(hl + plen));    /* total length         */
    wr16(pkt + 4, s_ip_id++);           /* identification       */
    wr16(pkt + 6, 0x4000);              /* flags: don't fragment*/
    pkt[8]  = 64;                        /* TTL                  */
    pkt[9]  = proto;
    wr32(pkt + 12, IP_LOCAL);            /* source               */
    wr32(pkt + 16, dst_ip);              /* destination          */
    u16 c = inet_cksum(pkt, hl);
    wr16(pkt + 10, c);                   /* header checksum      */
    if (plen > 0) memcpy(pkt + hl, payload, (unsigned)plen);
    /* On slirp every destination is reached via the gateway MAC. */
    eth_send(s_gw_mac, ETH_IPV4, pkt, hl + plen);
}

/* ── ICMP echo (ping) ────────────────────────────────────────────────────── */
static volatile int s_icmp_got = 0;
static u16 s_icmp_id = 0x4747, s_icmp_seq = 0;
static u32 s_icmp_from = 0;

static void icmp_handle(u32 src_ip, const u8* p, int len) {
    if (len < 8) return;
    u8 type = p[0];
    if (type == 0) {                     /* echo reply           */
        u16 id  = rd16(p + 4);
        u16 seq = rd16(p + 6);
        if (id == s_icmp_id && seq == s_icmp_seq) {
            s_icmp_got = 1; s_icmp_from = src_ip;
        }
    }
}

int net_ping(unsigned int ip, int timeout_ms) {
    if (!s_ready) return -1;
    if (!arp_resolve_gateway()) return -1;
    u8 pkt[64];
    memset(pkt, 0, sizeof(pkt));
    s_icmp_seq++;
    s_icmp_got = 0;
    pkt[0] = 8;                          /* echo request         */
    pkt[1] = 0;
    wr16(pkt + 4, s_icmp_id);
    wr16(pkt + 6, s_icmp_seq);
    for (int i = 8; i < 40; i++) pkt[i] = (u8)('a' + (i & 15));
    int icmp_len = 40;
    u16 c = inet_cksum(pkt, icmp_len);
    wr16(pkt + 2, c);

    unsigned long start = jiffies;
    ipv4_send(ip, IP_PROTO_ICMP, pkt, icmp_len);
    unsigned long deadline = jiffies + (unsigned)(timeout_ms / 10) + 1;
    while (jiffies < deadline) {
        net_poll();
        __asm__ volatile("hlt");   /* sleep until next IRQ (NIC RX or PIT) */
        if (s_icmp_got) {
            unsigned long rtt = (jiffies - start) * 10;
            return (int)rtt;
        }
    }
    return -1;
}

/* ── UDP (used by DNS) ───────────────────────────────────────────────────── */
static volatile int s_udp_got = 0;
static u16 s_udp_local_port = 0;
static u8  s_udp_rx[1024];
static int s_udp_rxlen = 0;

static void udp_handle(u32 src_ip, const u8* p, int len) {
    (void)src_ip;
    if (len < 8) return;
    u16 dport = rd16(p + 2);
    if (dport != s_udp_local_port) return;
    int dlen = (int)rd16(p + 4) - 8;
    if (dlen < 0) return;
    if (dlen > (int)sizeof(s_udp_rx)) dlen = (int)sizeof(s_udp_rx);
    if (dlen > len - 8) dlen = len - 8;
    memcpy(s_udp_rx, p + 8, (unsigned)dlen);
    s_udp_rxlen = dlen;
    s_udp_got = 1;
}

static void udp_send(u32 dst_ip, u16 sport, u16 dport, const u8* data, int len) {
    u8 seg[1024 + 8];
    if (len > 1024) len = 1024;
    wr16(seg + 0, sport);
    wr16(seg + 2, dport);
    wr16(seg + 4, (u16)(8 + len));
    wr16(seg + 6, 0);                    /* checksum optional in IPv4 */
    if (len > 0) memcpy(seg + 8, data, (unsigned)len);
    ipv4_send(dst_ip, IP_PROTO_UDP, seg, 8 + len);
}

/* ── DHCP client (DISCOVER → OFFER → REQUEST → ACK) ──────────────────────────
   Broadcast BOOTP/DHCP. Falls back to the static slirp config if no server
   answers, so networking always works. On success it updates s_ip/s_gw/s_dns. */

/* Raw broadcast send: IP 0.0.0.0 → 255.255.255.255, UDP 68 → 67, ETH broadcast.
   Cannot use ipv4_send (that routes via the gateway MAC we may not know yet). */
static void dhcp_send_raw(const u8* dhcp, int dlen) {
    u8 pkt[600];
    int ihl = 20, udpl = 8 + dlen, total = ihl + udpl;
    if (total > (int)sizeof(pkt)) return;
    memset(pkt, 0, ihl + 8);
    pkt[0] = 0x45; pkt[1] = 0;
    wr16(pkt + 2, (u16)total);
    wr16(pkt + 4, 0); wr16(pkt + 6, 0);
    pkt[8] = 64; pkt[9] = IP_PROTO_UDP;
    wr32(pkt + 12, 0x00000000u);          /* src 0.0.0.0        */
    wr32(pkt + 16, 0xFFFFFFFFu);          /* dst 255.255.255.255 */
    wr16(pkt + 10, inet_cksum(pkt, ihl));
    wr16(pkt + ihl + 0, 68);
    wr16(pkt + ihl + 2, 67);
    wr16(pkt + ihl + 4, (u16)udpl);
    wr16(pkt + ihl + 6, 0);
    if (dlen > 0) memcpy(pkt + ihl + 8, dhcp, (unsigned)dlen);
    static const u8 bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_send(bcast, ETH_IPV4, pkt, total);
}

static int dhcp_build(u8* b, u8 msgtype, u32 xid, u32 req_ip, u32 server_ip) {
    memset(b, 0, 240);
    b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;   /* op=request htype=eth hlen=6 */
    wr32(b + 4, xid);
    wr16(b + 10, 0x8000);                     /* broadcast flag              */
    mac_copy(b + 28, s_mac);                  /* chaddr                      */
    wr32(b + 236, 0x63825363u);               /* DHCP magic cookie           */
    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = msgtype;                  /* msg type    */
    b[o++] = 61; b[o++] = 7; b[o++] = 1; mac_copy(b + o, s_mac); o += 6; /* client id */
    if (msgtype == 3) {                         /* REQUEST carries these       */
        b[o++] = 50; b[o++] = 4; wr32(b + o, req_ip);    o += 4;  /* requested */
        b[o++] = 54; b[o++] = 4; wr32(b + o, server_ip); o += 4;  /* server id */
    }
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 15;
    b[o++] = 255;                               /* end                         */
    return o;
}

static const u8* dhcp_opt(const u8* m, int mlen, u8 code, int* olen) {
    int o = 240;
    while (o < mlen) {
        u8 c = m[o++];
        if (c == 255) break;
        if (c == 0) continue;
        if (o >= mlen) break;
        int l = m[o++];
        if (o + l > mlen) break;
        if (c == code) { *olen = l; return m + o; }
        o += l;
    }
    return 0;
}

/* Returns 1 and applies the lease on success, 0 if no server answered. */
static int dhcp_configure(void) {
    if (!s_ready) return 0;
    u32 xid = 0x47524547u ^ (u32)(jiffies << 8);
    s_dhcp_active   = 1;
    s_udp_local_port = 68;

    u8 msg[300];
    int len = dhcp_build(msg, 1, xid, 0, 0);      /* DISCOVER */
    u32 offered = 0, server = 0, gw = 0, dns = 0;
    int got_offer = 0;

    for (int tries = 0; tries < 3 && !got_offer; tries++) {
        s_udp_got = 0;
        dhcp_send_raw(msg, len);
        unsigned long deadline = jiffies + 100;   /* ~1 s */
        while (jiffies < deadline && !s_udp_got) { net_poll(); __asm__ volatile("hlt"); }
        if (!s_udp_got) continue;
        const u8* r = s_udp_rx; int rlen = s_udp_rxlen;
        if (rlen < 240 || rd32(r + 4) != xid) continue;
        int ol; const u8* t = dhcp_opt(r, rlen, 53, &ol);
        if (!t || ol < 1 || t[0] != 2) continue;  /* want an OFFER */
        offered = rd32(r + 16);                    /* yiaddr */
        const u8* sid = dhcp_opt(r, rlen, 54, &ol); if (sid && ol == 4) server = rd32(sid);
        const u8* g   = dhcp_opt(r, rlen, 3,  &ol); if (g   && ol >= 4) gw = rd32(g);
        const u8* d   = dhcp_opt(r, rlen, 6,  &ol); if (d   && ol >= 4) dns = rd32(d);
        got_offer = (offered != 0);
    }
    if (!got_offer) { s_dhcp_active = 0; return 0; }

    len = dhcp_build(msg, 3, xid, offered, server);   /* REQUEST */
    int got_ack = 0;
    for (int tries = 0; tries < 3 && !got_ack; tries++) {
        s_udp_got = 0;
        dhcp_send_raw(msg, len);
        unsigned long deadline = jiffies + 100;
        while (jiffies < deadline && !s_udp_got) { net_poll(); __asm__ volatile("hlt"); }
        if (!s_udp_got) continue;
        const u8* r = s_udp_rx; int rlen = s_udp_rxlen;
        if (rlen < 240 || rd32(r + 4) != xid) continue;
        int ol; const u8* t = dhcp_opt(r, rlen, 53, &ol);
        if (t && ol >= 1 && t[0] == 5) got_ack = 1;                 /* ACK */
        else if (t && ol >= 1 && t[0] == 6) { s_dhcp_active = 0; return 0; } /* NAK */
    }
    s_dhcp_active = 0;
    if (!got_ack) return 0;

    s_ip = offered;
    if (gw)  s_gw  = gw;
    if (dns) s_dns = dns;
    s_gw_mac_known = 0;   /* gateway may differ → re-ARP */
    return 1;
}

/* ── DNS ─────────────────────────────────────────────────────────────────── */
static int is_dotted_quad(const char* s) {
    int dots = 0, digits = 0;
    for (const char* p = s; *p; p++) {
        if (*p == '.') { dots++; digits = 0; }
        else if (*p >= '0' && *p <= '9') { if (++digits > 3) return 0; }
        else return 0;
    }
    return dots == 3;
}

unsigned int net_parse_ip(const char* s) {
    u32 ip = 0; int octet = 0, parts = 0, seen = 0;
    for (const char* p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { octet = octet * 10 + (*p - '0'); seen = 1; if (octet > 255) return 0; }
        else if (*p == '.' || *p == '\0') {
            if (!seen) return 0;
            ip = (ip << 8) | (u32)octet;
            parts++; octet = 0; seen = 0;
            if (*p == '\0') break;
        } else return 0;
    }
    return parts == 4 ? ip : 0;
}

void net_format_ip(unsigned int ip, char* buf) {
    int pos = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        int v = (int)((ip >> shift) & 0xFF);
        if (v >= 100) buf[pos++] = (char)('0' + v / 100);
        if (v >= 10)  buf[pos++] = (char)('0' + (v / 10) % 10);
        buf[pos++] = (char)('0' + v % 10);
        if (shift) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

unsigned int net_dns_resolve(const char* hostname) {
    if (!hostname || !hostname[0]) return 0;
    if (is_dotted_quad(hostname)) return net_parse_ip(hostname);
    if (!s_ready) return 0;
    if (!arp_resolve_gateway()) return 0;

    /* Build DNS query for an A record. */
    u8 q[512];
    memset(q, 0, sizeof(q));
    u16 txid = (u16)(0x1000 + (jiffies & 0x0FFF));
    wr16(q + 0, txid);
    wr16(q + 2, 0x0100);                 /* recursion desired      */
    wr16(q + 4, 1);                      /* QDCOUNT = 1            */
    int pos = 12;
    /* QNAME: length-prefixed labels */
    const char* h = hostname;
    while (*h) {
        int lblstart = pos++;
        int lbllen = 0;
        while (*h && *h != '.') {
            if (pos < (int)sizeof(q) - 8) q[pos++] = (u8)*h;
            h++; lbllen++;
        }
        q[lblstart] = (u8)lbllen;
        if (*h == '.') h++;
    }
    q[pos++] = 0;                         /* root label            */
    wr16(q + pos, 1); pos += 2;           /* QTYPE  A              */
    wr16(q + pos, 1); pos += 2;           /* QCLASS IN             */

    s_udp_local_port = (u16)(40000 + (jiffies & 0x3FFF));
    for (int tries = 0; tries < 3; tries++) {
        s_udp_got = 0; s_udp_rxlen = 0;
        udp_send(IP_DNS, s_udp_local_port, 53, q, pos);
        unsigned long deadline = jiffies + 200;      /* 2 s per try            */
        while (jiffies < deadline && !s_udp_got) { net_poll(); __asm__ volatile("hlt"); }
        if (!s_udp_got) continue;

        /* Parse the response. */
        const u8* r = s_udp_rx;
        int rlen = s_udp_rxlen;
        if (rlen < 12) continue;
        if (rd16(r) != txid) continue;
        int qd = rd16(r + 4), an = rd16(r + 6);
        if (an < 1) return 0;                        /* NXDOMAIN / no A         */
        int rp = 12;
        /* Skip the question section. */
        for (int i = 0; i < qd && rp < rlen; i++) {
            while (rp < rlen && r[rp] != 0) {
                if ((r[rp] & 0xC0) == 0xC0) { rp += 1; break; }
                rp += r[rp] + 1;
            }
            rp += 1;      /* zero label / second pointer byte */
            rp += 4;      /* QTYPE + QCLASS                   */
        }
        /* Walk answers, return the first A record. */
        for (int i = 0; i < an && rp + 12 <= rlen; i++) {
            if ((r[rp] & 0xC0) == 0xC0) rp += 2;
            else { while (rp < rlen && r[rp] != 0) rp += r[rp] + 1; rp += 1; }
            if (rp + 10 > rlen) break;
            u16 type = rd16(r + rp);
            u16 rdlen = rd16(r + rp + 8);
            rp += 10;
            if (type == 1 && rdlen == 4 && rp + 4 <= rlen) {
                return rd32(r + rp);
            }
            rp += rdlen;
        }
        return 0;
    }
    return 0;
}

/* ── TCP client ──────────────────────────────────────────────────────────── */
enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_CLOSE_WAIT, TCP_DEAD };

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_RXCAP 65536

typedef struct {
    int   state;
    u32   peer_ip;
    u16   local_port, peer_port;
    u32   snd_nxt;      /* next seq we will send                    */
    u32   rcv_nxt;      /* next seq we expect from peer             */
    u8*   rxbuf;        /* kmalloc'd receive buffer                 */
    int   rxhead;       /* bytes available to read                  */
    int   rxread;       /* consumer cursor                          */
    int   used;
} TcpSock;

#define TCP_NSOCK 4
static TcpSock s_tcp[TCP_NSOCK];

static void tcp_out(TcpSock* s, u8 flags, const u8* data, int dlen) {
    u8 seg[1500];
    int hl = 20;
    if (dlen > (int)sizeof(seg) - hl) dlen = (int)sizeof(seg) - hl;
    memset(seg, 0, hl);
    wr16(seg + 0, s->local_port);
    wr16(seg + 2, s->peer_port);
    wr32(seg + 4, s->snd_nxt);
    wr32(seg + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0);
    seg[12] = 0x50;                       /* data offset = 5 words  */
    seg[13] = flags;
    wr16(seg + 14, 8192);                /* window                 */
    wr16(seg + 16, 0);                   /* checksum (filled below)*/
    wr16(seg + 18, 0);                   /* urgent ptr             */
    if (dlen > 0) memcpy(seg + hl, data, (unsigned)dlen);

    /* TCP checksum with pseudo-header. */
    int seglen = hl + dlen;
    u8 pseudo[12];
    wr32(pseudo + 0, IP_LOCAL);
    wr32(pseudo + 4, s->peer_ip);
    pseudo[8] = 0; pseudo[9] = IP_PROTO_TCP;
    wr16(pseudo + 10, (u16)seglen);
    u32 sum = 0;
    for (int i = 0; i + 1 < 12; i += 2) sum += (u32)((pseudo[i] << 8) | pseudo[i+1]);
    int i = 0;
    for (; i + 1 < seglen; i += 2) sum += (u32)((seg[i] << 8) | seg[i+1]);
    if (i < seglen) sum += (u32)(seg[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    wr16(seg + 16, (u16)(~sum & 0xFFFF));

    ipv4_send(s->peer_ip, IP_PROTO_TCP, seg, seglen);
    if (flags & TCP_SYN) s->snd_nxt += 1;
    else if (flags & TCP_FIN) s->snd_nxt += 1;
    s->snd_nxt += (u32)dlen;
}

static void tcp_handle(u32 src_ip, const u8* p, int len) {
    if (len < 20) return;
    u16 dport = rd16(p + 2);
    TcpSock* s = 0;
    for (int i = 0; i < TCP_NSOCK; i++)
        if (s_tcp[i].used && s_tcp[i].local_port == dport &&
            s_tcp[i].peer_ip == src_ip) { s = &s_tcp[i]; break; }
    if (!s) return;

    u32 seq = rd32(p + 4);
    u32 ack = rd32(p + 8);
    u8  flags = p[13];
    int doff = (p[12] >> 4) * 4;
    if (doff < 20 || doff > len) return;
    int dlen = len - doff;
    const u8* data = p + doff;

    if (flags & TCP_RST) { s->state = TCP_DEAD; return; }

    if (s->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s->rcv_nxt = seq + 1;
            (void)ack;
            s->state = TCP_ESTABLISHED;
            tcp_out(s, TCP_ACK, 0, 0);       /* complete handshake     */
        }
        return;
    }

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        /* Accept in-order data only (no reassembly). */
        if (dlen > 0 && seq == s->rcv_nxt) {
            /* Reclaim/compact the buffer once the consumer has drained it,
               so arbitrarily large responses stream correctly instead of
               capping at TCP_RXCAP. */
            if (s->rxread == s->rxhead) { s->rxread = 0; s->rxhead = 0; }
            else if (s->rxread > 0) {
                int rem = s->rxhead - s->rxread;
                if (rem > 0) memcpy(s->rxbuf, s->rxbuf + s->rxread, (unsigned)rem);
                s->rxhead = rem; s->rxread = 0;
            }
            int space = TCP_RXCAP - s->rxhead;
            if (dlen <= space) {
                memcpy(s->rxbuf + s->rxhead, data, (unsigned)dlen);
                s->rxhead  += dlen;
                s->rcv_nxt += (u32)dlen;     /* advance ONLY for stored bytes */
            }
            /* If it did not fit, rcv_nxt is unchanged → a duplicate ACK; the
               peer still holds the data and retransmits. Nothing is lost. */
            tcp_out(s, TCP_ACK, 0, 0);
        } else if (dlen > 0) {
            /* Out-of-order / retransmit: re-ack what we have. */
            tcp_out(s, TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            if (s->state != TCP_CLOSE_WAIT) {   /* advance/transition once */
                s->rcv_nxt += 1;
                s->state = TCP_CLOSE_WAIT;      /* peer done sending       */
            }
            tcp_out(s, TCP_ACK, 0, 0);          /* always re-ack duplicates */
        }
    }
}

int tcp_connect(unsigned int ip, unsigned short port, int timeout_ms) {
    if (!s_ready) return -1;
    if (!arp_resolve_gateway()) return -1;
    int idx = -1;
    for (int i = 0; i < TCP_NSOCK; i++) if (!s_tcp[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    TcpSock* s = &s_tcp[idx];
    memset(s, 0, sizeof(*s));
    s->rxbuf = (u8*)kmalloc(TCP_RXCAP);
    if (!s->rxbuf) return -1;
    s->used = 1;
    s->peer_ip = ip;
    s->peer_port = port;
    s->local_port = (u16)(49152 + ((jiffies + idx) & 0x3FFF));  /* stays 49152..65535 */
    s->snd_nxt = (0x00C0FFEEu ^ (u32)(jiffies << 8));
    s->rcv_nxt = 0;
    s->state = TCP_SYN_SENT;

    tcp_out(s, TCP_SYN, 0, 0);
    unsigned long deadline = jiffies + (unsigned)(timeout_ms / 10) + 1;
    unsigned long resend = jiffies + 50;
    while (jiffies < deadline && s->state == TCP_SYN_SENT) {
        net_poll();
        __asm__ volatile("hlt");   /* sleep until next IRQ (NIC RX or PIT) */
        if (jiffies >= resend) { s->snd_nxt -= 1; tcp_out(s, TCP_SYN, 0, 0); resend = jiffies + 50; }
    }
    if (s->state != TCP_ESTABLISHED) { kfree(s->rxbuf); s->used = 0; s->state = TCP_CLOSED; return -1; }
    return idx;
}

int tcp_send(int sock, const void* data, int len) {
    if (sock < 0 || sock >= TCP_NSOCK || !s_tcp[sock].used) return -1;
    TcpSock* s = &s_tcp[sock];
    if (s->state != TCP_ESTABLISHED) return -1;
    const u8* p = (const u8*)data;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        tcp_out(s, TCP_ACK | TCP_PSH, p + sent, chunk);
        sent += chunk;
        net_poll();
        __asm__ volatile("hlt");   /* sleep until next IRQ (NIC RX or PIT) */
    }
    return sent;
}

int tcp_recv(int sock, void* buf, int maxlen, int timeout_ms) {
    if (sock < 0 || sock >= TCP_NSOCK || !s_tcp[sock].used) return -1;
    TcpSock* s = &s_tcp[sock];
    unsigned long deadline = jiffies + (unsigned)(timeout_ms / 10) + 1;
    while (jiffies < deadline) {
        if (s->rxhead > s->rxread) {
            int avail = s->rxhead - s->rxread;
            int n = avail < maxlen ? avail : maxlen;
            memcpy(buf, s->rxbuf + s->rxread, (unsigned)n);
            s->rxread += n;
            return n;
        }
        if (s->state == TCP_CLOSE_WAIT || s->state == TCP_DEAD) return 0;  /* EOF */
        net_poll();
        __asm__ volatile("hlt");   /* sleep until next IRQ (NIC RX or PIT) */
    }
    return -1;   /* timeout */
}

int tcp_is_open(int sock) {
    if (sock < 0 || sock >= TCP_NSOCK || !s_tcp[sock].used) return 0;
    TcpSock* s = &s_tcp[sock];
    if (s->rxhead > s->rxread) return 1;
    return s->state == TCP_ESTABLISHED;
}

void tcp_close(int sock) {
    if (sock < 0 || sock >= TCP_NSOCK || !s_tcp[sock].used) return;
    TcpSock* s = &s_tcp[sock];
    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)
        tcp_out(s, TCP_FIN | TCP_ACK, 0, 0);
    /* Give the peer a brief chance to ACK/FIN. */
    unsigned long deadline = jiffies + 20;
    while (jiffies < deadline && s->state != TCP_DEAD) { net_poll(); __asm__ volatile("hlt"); }
    if (s->rxbuf) kfree(s->rxbuf);
    s->rxbuf = 0; s->used = 0; s->state = TCP_CLOSED;
}

/* ── IPv4 receive dispatch ───────────────────────────────────────────────── */
static void ipv4_handle(const u8* p, int len) {
    if (len < 20) return;
    int ihl = (p[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    u16 total = rd16(p + 2);
    if (total > len) total = (u16)len;
    u8  proto = p[9];
    u32 src = rd32(p + 12);
    u32 dst = rd32(p + 16);
    /* Normally accept only our IP or broadcast; while negotiating DHCP we have
       no confirmed IP yet, so accept anything (the DHCP handler filters). */
    if (!s_dhcp_active && dst != IP_LOCAL && dst != 0xFFFFFFFFu) return;
    const u8* payload = p + ihl;
    int plen = (int)total - ihl;
    if (plen < 0) return;
    switch (proto) {
        case IP_PROTO_ICMP: icmp_handle(src, payload, plen); break;
        case IP_PROTO_UDP:  udp_handle(src, payload, plen);  break;
        case IP_PROTO_TCP:  tcp_handle(src, payload, plen);  break;
        default: break;
    }
}

static void eth_handle(const u8* frame, int len) {
    if (len < 14) return;
    u16 ethertype = rd16(frame + 12);
    const u8* payload = frame + 14;
    int plen = len - 14;
    if (ethertype == ETH_ARP)       arp_handle(payload, plen);
    else if (ethertype == ETH_IPV4) ipv4_handle(payload, plen);
}

/* ── IRQ-driven RX (roadmap Phase 9.1) ───────────────────────────────────────
   The RTL8139 raises its PCI interrupt on ROK; irq_net_handler drains the
   hardware ring into a lock-free software queue, and net_poll() — called from
   mainline (idle loop + blocking helpers) — pops frames and runs the protocol
   stack. Split of responsibility:

     ISR (irq_net_handler)          mainline (net_poll)
     ── owns the HW ring, CAPR,     ── owns ALL protocol state (TCP socks,
        s_rx_offset, RTL_ISR ack       DNS/DHCP/ICMP, TX path)
     ── copies frames → s_rxq       ── pops s_rxq → eth_handle()

   The queue is single-producer/single-consumer (ISR/mainline) on one CPU:
   volatile indices + aligned 32-bit accesses need no lock (same discipline
   as drivers/event.cpp). A full queue drops the frame — TCP retransmits.   */

#define RX_QUEUE_N   32
#define RX_SLOT_MAX  1600                 /* matches the length>1600 reject   */
struct rx_slot { u16 len; u8 data[RX_SLOT_MAX]; };
static struct rx_slot        s_rxq[RX_QUEUE_N];
static volatile unsigned int s_rxq_head;  /* written by the ISR (producer)    */
static volatile unsigned int s_rxq_tail;  /* written by net_poll (consumer)   */
static u8 s_net_irq;                      /* PIC line from PCI config 0x3C    */

/* Drain the hardware ring into s_rxq. ISR context only (or mainline with
   interrupts disabled — see the safety net in net_poll).                   */
static void net_rx_hw_drain(void) {
    int guard = 0;
    while (!(port_byte_in(s_iobase + RTL_CMD) & CMD_BUFE)) {
        if (++guard > 64) break;                     /* fairness cap per call  */
        u8* base = s_rxbuf;
        u32 off = s_rx_offset % 8192;
        u16 status = (u16)(base[off] | (base[off + 1] << 8));
        u16 length = (u16)(base[off + 2] | (base[off + 3] << 8));

        if (!(status & RX_ROK) || length < 14 || length > 1600) {
            /* Corrupt: reset the receiver to recover. */
            port_byte_out(s_iobase + RTL_CMD, CMD_TE);
            s_rx_offset = 0;
            port_word_out(s_iobase + RTL_CAPR, (u16)(s_rx_offset - 16));
            port_byte_out(s_iobase + RTL_CMD, CMD_TE | CMD_RE);
            break;
        }

        int fl = length - 4;                         /* strip 4-byte CRC       */
        unsigned int next = (s_rxq_head + 1) % RX_QUEUE_N;
        if (next != s_rxq_tail) {                    /* queue not full         */
            struct rx_slot* slot = &s_rxq[s_rxq_head];
            u32 pstart = (off + 4) % 8192;
            int copy = fl < RX_SLOT_MAX ? fl : RX_SLOT_MAX;
            for (int i = 0; i < copy; i++)
                slot->data[i] = base[(pstart + i) % 8192];
            slot->len = (u16)copy;
            s_rxq_head = next;                       /* publish AFTER the copy */
        }                                            /* full → drop (TCP retx) */
        s_rx_packets++; s_rx_bytes += (u32)fl;

        /* Advance to next packet: 4-byte header + length, dword-aligned. */
        s_rx_offset = (s_rx_offset + length + 4 + 3) & ~3u;
        s_rx_offset %= 8192;
        port_word_out(s_iobase + RTL_CAPR, (u16)(s_rx_offset - 16));
    }
}

/* RTL8139 interrupt handler (called from irq_net_stub, arch/i386/isr.asm).
   Order matters: drain the ring, THEN ack RTL_ISR (write-1-to-clear with the
   read-back value), THEN EOI the PIC(s) — acking after EOI or EOI without
   acking leaves the line asserted and storms.                               */
void irq_net_handler(void) {
    if (!s_ready) { port_byte_out(0x20, 0x20); return; }
    u16 isr = port_word_in(s_iobase + RTL_ISR);
    if (isr & RX_ROK) net_rx_hw_drain();
    if (isr) port_word_out(s_iobase + RTL_ISR, isr);
    if (s_net_irq >= 8) port_byte_out(0xA0, 0x20);   /* slave EOI first        */
    port_byte_out(0x20, 0x20);                       /* master EOI             */
}

/* Mainline consumer: pop queued frames and run the protocol stack. Also a
   lost-IRQ safety net: every ~10 jiffies, drain the hardware ring directly
   (interrupts masked so the ISR can't race the ring pointers).             */
void net_poll(void) {
    if (!s_ready) return;

    static unsigned long last_hw_check = 0;
    if (jiffies - last_hw_check >= 10) {
        last_hw_check = jiffies;
        __asm__ volatile("cli");
        if (!(port_byte_in(s_iobase + RTL_CMD) & CMD_BUFE)) net_rx_hw_drain();
        __asm__ volatile("sti");
    }

    while (s_rxq_tail != s_rxq_head) {
        struct rx_slot* slot = &s_rxq[s_rxq_tail];
        static u8 frame[RX_SLOT_MAX];
        int fl = slot->len;
        for (int i = 0; i < fl; i++) frame[i] = slot->data[i];
        s_rxq_tail = (s_rxq_tail + 1) % RX_QUEUE_N;  /* release slot, THEN handle */
        eth_handle(frame, fl);
    }
}

/* ── init ────────────────────────────────────────────────────────────────── */
int net_init(void) {
    if (s_ready) return 1;
    u8 bus, slot;
    if (!pci_find_rtl8139(&bus, &slot)) {
        term_print("[net] Aucune carte RTL8139 detectee (PCI)\n");
        return 0;
    }

    /* Enable I/O space + bus mastering in the PCI command register. */
    u32 cmd = pci_read32(bus, slot, 0, 0x04);
    cmd |= 0x05;                                     /* IO space | bus master  */
    pci_write32(bus, slot, 0, 0x04, cmd);

    /* BAR0 = I/O base (low bit set → I/O-mapped). */
    u32 bar0 = pci_read32(bus, slot, 0, 0x10);
    s_iobase = (u16)(bar0 & ~0x3u);

    /* Power on, then soft-reset. */
    port_byte_out(s_iobase + RTL_CONFIG1, 0x00);
    port_byte_out(s_iobase + RTL_CMD, CMD_RST);
    for (int spin = 0; spin < 1000000; spin++)
        if (!(port_byte_in(s_iobase + RTL_CMD) & CMD_RST)) break;

    /* Read MAC from IDR0..5. */
    for (int i = 0; i < 6; i++) s_mac[i] = port_byte_in(s_iobase + RTL_IDR0 + i);

    /* Allocate the RX ring (8 KB + 16-byte header slack + 1536 wrap pad). */
    s_rxbuf = (u8*)kmalloc(8192 + 16 + 1536);
    if (!s_rxbuf) { term_print("[net] RX buffer alloc echouee\n"); return 0; }
    memset(s_rxbuf, 0, 8192 + 16 + 1536);
    for (int i = 0; i < 4; i++) {
        s_txbuf[i] = (u8*)kmalloc(2048);
        if (!s_txbuf[i]) { term_print("[net] TX buffer alloc echouee\n"); return 0; }
    }

    port_dword_out(s_iobase + RTL_RBSTART, (u32)(unsigned long)s_rxbuf);
    /* IMR: ROK only. TX completion is spin-waited in eth_send, so TOK would
       just cost one interrupt per transmitted frame for nothing.           */
    port_word_out(s_iobase + RTL_IMR, 0x0001);
    /* RCR: accept broadcast + our MAC + multicast, 8 KB, no threshold.
       WRAP=0 (bit 7 clear) so the NIC wraps packets at the 8 KB boundary —
       this matches net_rx_hw_drain()'s `% 8192` body copy. (Was 0x8F, i.e.
       WRAP=1, which made the chip write boundary-straddling packets into the
       slack pad while the copy still wrapped at 8 KB → corrupted large RX.)  */
    port_dword_out(s_iobase + RTL_RCR, 0x0000000F);
    port_byte_out(s_iobase + RTL_CMD, CMD_TE | CMD_RE);
    s_rx_offset = 0;
    port_word_out(s_iobase + RTL_CAPR, (u16)(0 - 16));

    s_ready = 1;

    /* IRQ-driven RX: read the routed PIC line from PCI config 0x3C, install
       the interrupt gate (master base 0x20 / slave base 0x28 — matches the
       PIC remap in kernel.c), then unmask that line. Read-modify-write the
       mask registers so the timer/keyboard/mouse lines are untouched.      */
    s_net_irq = (u8)(pci_read32(bus, slot, 0, 0x3C) & 0xFF);
    if (s_net_irq > 0 && s_net_irq < 16) {
        unsigned int vector = (s_net_irq < 8) ? (0x20u + s_net_irq)
                                              : (0x28u + (s_net_irq - 8u));
        kernel_net_irq_install(vector);
        if (s_net_irq >= 8) {
            u8 m = port_byte_in(0xA1);
            port_byte_out(0xA1, (u8)(m & ~(1u << (s_net_irq - 8))));
        } else {
            u8 m = port_byte_in(0x21);
            port_byte_out(0x21, (u8)(m & ~(1u << s_net_irq)));
        }
    }

    term_print("[net] RTL8139 pret (RX par IRQ ");
    term_print_int((int)s_net_irq);
    term_print(")  MAC ");
    for (int i = 0; i < 6; i++) {
        const char* hx = "0123456789ABCDEF";
        char b[3]; b[0] = hx[s_mac[i] >> 4]; b[1] = hx[s_mac[i] & 15]; b[2] = 0;
        term_print(b); if (i < 5) term_print(":");
    }
    term_print("\n");

    /* Try DHCP; keep the static slirp defaults if no server answers. */
    if (dhcp_configure())
        term_print("[net] DHCP: bail obtenu  ");
    else
        term_print("[net] DHCP indisponible, config statique  ");
    {
        char ips[16], gws[16], dns[16];
        net_format_ip(s_ip, ips); net_format_ip(s_gw, gws); net_format_ip(s_dns, dns);
        term_print("IP "); term_print(ips);
        term_print("  GW "); term_print(gws);
        term_print("  DNS "); term_print(dns); term_print("\n");
    }

    /* Prime the gateway ARP entry so the first ping/DNS isn't slow. */
    arp_resolve_gateway();
    return 1;
}

int net_ready(void)            { return s_ready; }
unsigned int net_local_ip(void)   { return s_ready ? IP_LOCAL : 0; }
unsigned int net_gateway_ip(void) { return IP_GATEWAY; }
unsigned int net_dns_ip(void)     { return IP_DNS; }
void net_get_mac(unsigned char mac[6]) { for (int i = 0; i < 6; i++) mac[i] = s_mac[i]; }
unsigned int net_rx_packets(void) { return s_rx_packets; }
unsigned int net_tx_packets(void) { return s_tx_packets; }
unsigned int net_rx_bytes(void)   { return s_rx_bytes; }
unsigned int net_tx_bytes(void)   { return s_tx_bytes; }

/* ── HTTP/1.0 client ─────────────────────────────────────────────────────── */
static void str_copy_bounded(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Parse "http://host[:port]/path" → components. Returns 1 on success. */
static int http_parse_url(const char* url, char* host, int hostcap,
                          int* port, char* path, int pathcap) {
    const char* p = url;
    if (!(p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/'))
        return 0;
    p += 7;
    int hi = 0;
    *port = 80;
    while (*p && *p != '/' && *p != ':' && hi < hostcap - 1) host[hi++] = *p++;
    host[hi] = '\0';
    if (*p == ':') {
        p++; int pn = 0;
        while (*p >= '0' && *p <= '9') { pn = pn * 10 + (*p - '0'); p++; }
        if (pn > 0 && pn < 65536) *port = pn;
    }
    if (*p == '\0') { str_copy_bounded(path, "/", pathcap); }
    else str_copy_bounded(path, p, pathcap);
    return 1;
}

/* Case-insensitive header find within a text buffer (returns value start). */
static const char* find_header(const char* buf, int len, const char* name) {
    int nl = strlen(name);
    for (int i = 0; i + nl < len; i++) {
        int match = 1;
        for (int j = 0; j < nl; j++) {
            char a = buf[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) {
            const char* v = buf + i + nl;
            while (*v == ' ' || *v == ':') v++;
            return v;
        }
    }
    return 0;
}

/* Does header `name` carry `tok` (case-insensitive) in its value line? */
static int header_has_token(const char* buf, int len, const char* name, const char* tok) {
    const char* v = find_header(buf, len, name);
    if (!v) return 0;
    int tl = strlen(tok);
    for (int i = 0; v[i] && v[i] != '\r' && v[i] != '\n'; i++) {
        int m = 1;
        for (int j = 0; j < tl; j++) {
            char a = v[i+j], b = tok[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { m = 0; break; }
        }
        if (m) return 1;
    }
    return 0;
}

/* Decode HTTP/1.1 chunked transfer-encoding in place. Returns decoded length. */
static int http_dechunk(char* buf, int len) {
    int in = 0, out = 0;
    while (in < len) {
        unsigned int size = 0; int any = 0, digits = 0;
        while (in < len) {
            char ch = buf[in]; int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else break;
            if (digits < 7) size = size * 16u + (unsigned)d;   /* cap 7 hex → no overflow */
            any = 1; in++; digits++;
        }
        if (!any) break;
        while (in < len && buf[in] != '\n') in++;
        if (in < len) in++;
        if (size == 0) break;
        unsigned int avail = (unsigned)(len - in);              /* len-in >= 0 here */
        if (size > avail) size = avail;                          /* overflow-safe clamp */
        for (unsigned int i = 0; i < size; i++) buf[out++] = buf[in++];
        if (in < len && buf[in] == '\r') in++;
        if (in < len && buf[in] == '\n') in++;
    }
    return out;
}

int http_get(const char* url, char** out_body, int* out_len, int max_len,
             char* final_url, char* content_type) {
    if (out_body) *out_body = 0;
    if (out_len) *out_len = 0;
    if (content_type) content_type[0] = '\0';
    if (!s_ready) return 0;

    char cur_url[512];
    str_copy_bounded(cur_url, url, sizeof(cur_url));

    for (int redirect = 0; redirect < 6; redirect++) {
        char host[256], path[512];
        int port;
        if (!http_parse_url(cur_url, host, sizeof(host), &port, path, sizeof(path)))
            return -1;                       /* unsupported (e.g. https://)     */

        u32 ip = net_dns_resolve(host);
        if (!ip) return 0;

        int sock = tcp_connect(ip, (u16)port, 8000);
        if (sock < 0) return 0;

        /* Build the request. */
        char req[900];
        int rp = 0;
        const char* pfx = "GET ";
        for (int i = 0; pfx[i]; i++) req[rp++] = pfx[i];
        for (int i = 0; path[i] && rp < (int)sizeof(req) - 200; i++) req[rp++] = path[i];
        const char* mid = " HTTP/1.1\r\nHost: ";
        for (int i = 0; mid[i]; i++) req[rp++] = mid[i];
        for (int i = 0; host[i] && rp < (int)sizeof(req) - 80; i++) req[rp++] = host[i];
        const char* tail = "\r\nUser-Agent: GregNet/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        for (int i = 0; tail[i]; i++) req[rp++] = tail[i];

        tcp_send(sock, req, rp);

        /* Read the whole response into a growable-ish fixed buffer. */
        int cap = max_len + 2048;
        char* raw = (char*)kmalloc((unsigned)cap);
        if (!raw) { tcp_close(sock); return 0; }
        int total = 0;
        for (;;) {
            int n = tcp_recv(sock, raw + total, cap - 1 - total, 8000);
            if (n <= 0) break;               /* EOF or timeout                  */
            total += n;
            if (total >= cap - 1) break;
        }
        raw[total] = '\0';
        tcp_close(sock);

        if (total < 12) { kfree(raw); return 0; }

        /* Parse status line: "HTTP/1.x SSS ..." */
        int status = 0;
        {
            const char* sp = raw;
            while (*sp && *sp != ' ') sp++;
            while (*sp == ' ') sp++;
            for (int i = 0; i < 3 && sp[i] >= '0' && sp[i] <= '9'; i++)
                status = status * 10 + (sp[i] - '0');
        }

        /* Find header/body split. */
        int body_off = -1;
        for (int i = 0; i + 3 < total; i++) {
            if (raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n') { body_off = i + 4; break; }
            if (raw[i]=='\n'&&raw[i+1]=='\n') { body_off = i + 2; break; }
        }
        int header_len = (body_off < 0) ? total : body_off;

        /* Follow redirects (http→http only). */
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308)) {
            const char* loc = find_header(raw, header_len, "location");
            if (loc) {
                char newurl[512]; int k = 0;
                while (loc[k] && loc[k] != '\r' && loc[k] != '\n' && k < (int)sizeof(newurl) - 1) {
                    newurl[k] = loc[k]; k++;
                }
                newurl[k] = '\0';
                kfree(raw);
                if (newurl[0] == 'h' && newurl[4] == ':') {           /* absolute http(s) */
                    if (newurl[4]==':'&&newurl[5]=='/'&&newurl[6]=='/'&&newurl[3]=='s')
                        return -1;                                    /* https → give up  */
                    str_copy_bounded(cur_url, newurl, sizeof(cur_url));
                    continue;
                }
                return -1;
            }
        }

        /* Content-Type. */
        if (content_type) {
            const char* ct = find_header(raw, header_len, "content-type");
            if (ct) {
                int k = 0;
                while (ct[k] && ct[k] != '\r' && ct[k] != '\n' && ct[k] != ';' && k < 63) {
                    content_type[k] = ct[k]; k++;
                }
                content_type[k] = '\0';
            }
        }

        /* Copy body out to its own kmalloc buffer (decoding chunked if needed). */
        int blen = (body_off < 0) ? 0 : (total - body_off);
        if (body_off >= 0 && header_has_token(raw, header_len, "transfer-encoding", "chunked"))
            blen = http_dechunk(raw + body_off, blen);
        if (blen > max_len) blen = max_len;
        char* body = (char*)kmalloc((unsigned)(blen + 1));
        if (!body) { kfree(raw); return 0; }
        if (blen > 0) memcpy(body, raw + body_off, (unsigned)blen);
        body[blen] = '\0';
        kfree(raw);

        if (out_body) *out_body = body; else kfree(body);
        if (out_len) *out_len = blen;
        if (final_url) str_copy_bounded(final_url, cur_url, 512);
        return status ? status : 200;
    }
    return 0;   /* too many redirects */
}
