/* tls.c — GregOS TLS 1.2 client (ECDHE x25519/P-256, AES-128-GCM-SHA256).
   Brings HTTPS to the GregNet browser. The server is AUTHENTICATED: the
   certificate chain is verified against an embedded root store (hostname +
   validity + trust), and the ServerKeyExchange signature is checked against the
   leaf key — together these give real anti-MITM protection.
   Freestanding: no libc. Uses net.h (TCP) + crypto.h + certverify.h.          */

#include "../include/tls.h"
#include "../include/net.h"
#include "../include/crypto.h"
#include "../include/certverify.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

extern void*  kmalloc(unsigned int size);
extern void   kfree(void* p);
extern volatile unsigned long jiffies;
extern unsigned char get_rtc_register(int reg);   /* CMOS RTC (kernel.c) */
extern int           bcd_to_bin(unsigned char bcd);

/* current time as YYYYMMDDHHMMSS from the RTC, for certificate validity.
   Returns 0 (→ skip date check) if the clock is obviously unset, so a
   misconfigured VM clock can't brick HTTPS — chain/hostname/signature still
   enforced. */
static u64 rtc_now(void){
    while(get_rtc_register(0x0A)&0x80){}            /* wait out update-in-progress */
    /* Status register B bit 2 (DM): 1 = registers are binary, 0 = BCD. Assuming
       BCD unconditionally silently garbles the year on binary-mode clocks.   */
    int binary = (get_rtc_register(0x0B) & 0x04) != 0;
    #define RTCV(reg) ((u64)(binary ? (int)get_rtc_register(reg) : bcd_to_bin(get_rtc_register(reg))))
    u64 ss=RTCV(0x00), mm=RTCV(0x02), hh=RTCV(0x04);
    u64 day=RTCV(0x07), mon=RTCV(0x08), yr=2000+RTCV(0x09);
    #undef RTCV
    if(yr<2020||yr>2100||mon<1||mon>12||day<1||day>31||hh>23||mm>59||ss>60) return 0;
    return ((((((yr*100+mon)*100+day)*100+hh)*100+mm)*100)+ss);
}

/* last certificate-verification error (for the browser error page) */
static char s_cert_err[64] = "";
static void set_cert_err(const char* s){ int i=0; while(s[i]&&i<63){s_cert_err[i]=s[i];i++;} s_cert_err[i]='\0'; }
const char* tls_cert_error(void){ return s_cert_err[0]?s_cert_err:"certificat invalide"; }

/* ── debug channel: QEMU 0xE9 debugcon (enable with -debugcon file:...) ─────
   Flip to 1 + run QEMU with `-debugcon file:trace.txt` to get a full TLS
   handshake trace (harmless when 0, and when 1 with no debugcon attached).  */
#define TLS_DEBUG 0
static inline void outb(u16 p, u8 v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
#if TLS_DEBUG
static void dbg(const char* s){ while(*s) outb(0xE9,(u8)*s++); }
static void dbghex(const char* label, const u8* b, int n){
    static const char* H="0123456789abcdef";
    dbg(label); dbg(": ");
    for(int i=0;i<n;i++){ outb(0xE9,H[b[i]>>4]); outb(0xE9,H[b[i]&15]); }
    outb(0xE9,'\n');
}
static void dbgint(const char* label, int v){
    char t[16]; int i=0; dbg(label); dbg("=");
    if(v<0){ outb(0xE9,'-'); v=-v; }
    if(v==0) t[i++]='0';
    while(v){ t[i++]=(char)('0'+v%10); v/=10; }
    while(i) outb(0xE9,t[--i]);
    outb(0xE9,'\n');
}
#else
static void dbg(const char* s){ (void)s; }
static void dbghex(const char* l, const u8* b, int n){ (void)l;(void)b;(void)n; }
static void dbgint(const char* l, int v){ (void)l;(void)v; }
#endif

/* ── tiny helpers ─────────────────────────────────────────────────────────── */
static void t_memcpy(void* d,const void* s,u32 n){u8*dd=(u8*)d;const u8*ss=(const u8*)s;while(n--)*dd++=*ss++;}
static void t_memset(void* d,int v,u32 n){u8*dd=(u8*)d;while(n--)*dd++=(u8)v;}
static int  t_strlen(const char* s){int n=0;while(s[n])++n;return n;}
static void put16(u8* p,u32 v){p[0]=(u8)(v>>8);p[1]=(u8)v;}
static void put64(u8* p,u64 v){for(int i=0;i<8;i++)p[i]=(u8)(v>>(56-8*i));}

static inline u64 rdtsc(void){u32 lo,hi;__asm__ __volatile__("rdtsc":"=a"(lo),"=d"(hi));return((u64)hi<<32)|lo;}

/* ── RDRAND (hardware entropy), detected via CPUID.1:ECX bit 30 ──────────── */
static int g_rdrand = -1;                       /* -1 = not yet probed */
static int have_rdrand(void){
    if(g_rdrand>=0) return g_rdrand;
    u32 a,b,c,d;
    __asm__ __volatile__("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1u),"c"(0u));
    g_rdrand=(int)((c>>30)&1u);
    return g_rdrand;
}
static int rdrand32(u32* v){
    if(!have_rdrand()) return 0;
    for(int i=0;i<10;i++){                      /* Intel: retry a bounded number */
        u32 r; u8 ok;
        __asm__ __volatile__("rdrand %0; setc %1":"=r"(r),"=qm"(ok)::"cc");
        if(ok){ *v=r; return 1; }
    }
    return 0;
}

/* CSPRNG: a SHA-256 forward-chained pool stirred with hardware entropy.
   The pool is never emitted directly — output = SHA256(pool||0xFF) — so seeing
   client_random does not reveal the state used for the ECDHE private key.
   RDRAND supplies real entropy where the CPU has it (QEMU: use -cpu max/host);
   without it we fall back to TSC/jiffies, which is NOT strong — see README.  */
static u32 g_rctr = 0x1234u;
static u8  g_pool[32];
static int g_pool_ready = 0;

static void pool_stir(void){
    u8 sb[64];
    t_memcpy(sb,g_pool,32);                     /* carry the old state forward */
    put64(sb+32,rdtsc());
    put64(sb+40,(u64)jiffies);
    u32 r1=0,r2=0; rdrand32(&r1); rdrand32(&r2);
    put64(sb+48,((u64)r1<<32)|(u64)r2);
    put64(sb+56,(u64)g_rctr++ * 0x9E3779B97F4A7C15ull);
    sha256(sb,64,g_pool);                       /* pool = SHA256(pool || fresh) */
}

static void tls_rand(u8* out,int n){
    if(!g_pool_ready){
        u8 sb[48]; int k=0;
        put64(sb+k,rdtsc()); k+=8;
        put64(sb+k,(u64)jiffies); k+=8;
        for(int i=0;i<8;i++){                   /* 256 bits of RDRAND if present */
            u32 r=0; rdrand32(&r);
            sb[k++]=(u8)(r>>24); sb[k++]=(u8)(r>>16); sb[k++]=(u8)(r>>8); sb[k++]=(u8)r;
        }
        sha256(sb,(u32)k,g_pool); g_pool_ready=1;
        dbg(have_rdrand()?"[tls] CSPRNG seeded (RDRAND present)\n"
                         :"[tls] CSPRNG seeded WITHOUT RDRAND - weak entropy\n");
    }
    int filled=0;
    while(filled<n){
        pool_stir();
        u8 ob[33]; t_memcpy(ob,g_pool,32); ob[32]=0xFF;
        u8 h[32]; sha256(ob,33,h);              /* output is one-way from pool */
        int take=n-filled; if(take>32)take=32;
        t_memcpy(out+filled,h,take); filled+=take;
    }
}

/* ── record buffers (single connection at a time) ─────────────────────────── */
#define REC_MAX   18500        /* max TLSCiphertext incl. header slack       */
#define HS_MAX    32768        /* server handshake flight accumulation       */
static u8 s_rec[REC_MAX];      /* raw record payload (post-header)           */
static u8 s_plain[REC_MAX];    /* decrypted / plaintext record payload       */
static u8 s_hs[HS_MAX];        /* accumulated server handshake messages      */
static u8 s_out[REC_MAX];      /* outgoing (encrypted) record                */

typedef struct {
    int  sock;
    u8   client_random[32];
    u8   server_random[32];
    u8   server_pub[64];      /* x25519: 32 | secp256r1: 64 (X||Y) */
    u16  curve;               /* 0x001D x25519 | 0x0017 secp256r1  */
    u8   master[48];
    u8   client_key[16], server_key[16];
    u8   client_iv[4],   server_iv[4];
    u64  client_seq, server_seq;
    int  client_enc, server_enc;
    u16  cipher;
    sha256_ctx transcript;
    /* captured certificate chain (pointers into s_hs) + SKE signature, for
       authentication after the flight completes */
#define MAX_CERTS 10
    const u8* certs[MAX_CERTS]; int cert_lens[MAX_CERTS]; int cert_count;
    const u8* ske_params; int ske_params_len;   /* signed ServerECDHParams   */
    const u8* ske_sig;    int ske_sig_len;
    u8   ske_hash_alg, ske_sig_alg;              /* SignatureAndHashAlgorithm */
} tls_conn;

/* ── low-level record I/O ─────────────────────────────────────────────────── */

static int read_full(int sock, u8* buf, int n){
    int got=0;
    while(got<n){
        int r=tcp_recv(sock,buf+got,n-got,8000);
        if(r<=0) return got;              /* 0 = EOF, -1 = timeout; return partial */
        got+=r;
    }
    return got;
}

/* Read one TLS record → *type, plaintext into out (len *plen).
   Returns 1 ok, 0 clean EOF, -1 error.                                      */
static int read_record(tls_conn* c, int* type, u8* out, int* plen){
    u8 hdr[5];
    int r=read_full(c->sock,hdr,5);
    if(r==0) return 0;
    if(r<5)  return -1;
    int len=(hdr[3]<<8)|hdr[4];
    /* Reject empty records: `len` is built from two bytes so it can never be
       negative — a zero-length record would consume no bytes and let a hostile
       server spin the flight loop forever (no forward progress).             */
    if(len<=0||len>REC_MAX) return -1;
    if(read_full(c->sock,s_rec,len)<len) return -1;
    *type=hdr[0];

    if(c->server_enc){
        if(len<8+16) return -1;
        int ctlen=len-8-16;
        u8 nonce[12]; t_memcpy(nonce,c->server_iv,4); t_memcpy(nonce+4,s_rec,8);
        u8 aad[13]; put64(aad,c->server_seq);
        aad[8]=(u8)*type; aad[9]=0x03; aad[10]=0x03; put16(aad+11,(u32)ctlen);
        int ok=aes128_gcm_decrypt(c->server_key,nonce,aad,13,
                                  s_rec+8,ctlen,s_rec+8+ctlen,out);
        c->server_seq++;
        if(!ok){ dbg("[tls] bad tag on decrypt\n"); return -1; }
        *plen=ctlen;
    }else{
        t_memcpy(out,s_rec,len);
        *plen=len;
    }
    return 1;
}

static int send_plain_record(int sock,int type,const u8* data,int len){
    u8 hdr[5]; hdr[0]=(u8)type; hdr[1]=0x03; hdr[2]=0x03; put16(hdr+3,(u32)len);
    if(tcp_send(sock,hdr,5)<0) return -1;
    if(len && tcp_send(sock,data,len)<0) return -1;
    return 0;
}

static int send_enc_record(tls_conn* c,int type,const u8* plain,int plen){
    u8 nonce[12]; t_memcpy(nonce,c->client_iv,4); put64(nonce+4,c->client_seq);
    u8 aad[13]; put64(aad,c->client_seq);
    aad[8]=(u8)type; aad[9]=0x03; aad[10]=0x03; put16(aad+11,(u32)plen);
    /* record = hdr(5) || explicit_nonce(8) || ct(plen) || tag(16) */
    s_out[0]=(u8)type; s_out[1]=0x03; s_out[2]=0x03; put16(s_out+3,(u32)(8+plen+16));
    put64(s_out+5,c->client_seq);                       /* explicit nonce = seq */
    u8 tag[16];
    aes128_gcm_encrypt(c->client_key,nonce,aad,13,plain,(u32)plen,s_out+13,tag);
    t_memcpy(s_out+13+plen,tag,16);
    c->client_seq++;
    return tcp_send(c->sock,s_out,5+8+plen+16)<0 ? -1 : 0;
}

/* ── ClientHello ──────────────────────────────────────────────────────────── */
static int send_client_hello(tls_conn* c,const char* host){
    u8 h[512];
    /* handshake header filled at the end (need length) — reserve 4 bytes */
    int hs_start=4;
    int q=hs_start;
    h[q++]=0x03; h[q++]=0x03;                            /* client_version 1.2 */
    tls_rand(c->client_random,32);
    t_memcpy(h+q,c->client_random,32); q+=32;
    h[q++]=0x00;                                         /* session_id len 0   */
    /* cipher suites: ECDHE_RSA / ECDHE_ECDSA with AES_128_GCM_SHA256 */
    h[q++]=0x00; h[q++]=0x04;
    h[q++]=0xC0; h[q++]=0x2F;                            /* ECDHE_RSA_AES128_GCM   */
    h[q++]=0xC0; h[q++]=0x2B;                            /* ECDHE_ECDSA_AES128_GCM */
    h[q++]=0x01; h[q++]=0x00;                            /* compression: null  */
    /* extensions */
    int ext_len_at=q; q+=2;                              /* extensions length  */
    int hlen=t_strlen(host);
    /* SNI */
    h[q++]=0x00; h[q++]=0x00;                            /* server_name        */
    put16(h+q, (u32)(hlen+5)); q+=2;
    put16(h+q, (u32)(hlen+3)); q+=2;                     /* server_name_list   */
    h[q++]=0x00;                                         /* host_name type     */
    put16(h+q,(u32)hlen); q+=2;
    t_memcpy(h+q,host,(u32)hlen); q+=hlen;
    /* supported_groups: x25519 (preferred) + secp256r1 (P-256, for CDNs) */
    h[q++]=0x00; h[q++]=0x0A; h[q++]=0x00; h[q++]=0x06;   /* ext, len 6 */
    h[q++]=0x00; h[q++]=0x04;                             /* list len 4 */
    h[q++]=0x00; h[q++]=0x1D;                             /* x25519     */
    h[q++]=0x00; h[q++]=0x17;                             /* secp256r1  */
    /* ec_point_formats: uncompressed */
    h[q++]=0x00; h[q++]=0x0B; h[q++]=0x00; h[q++]=0x02; h[q++]=0x01; h[q++]=0x00;
    /* signature_algorithms — ONLY schemes we can actually verify (we now check
       the ServerKeyExchange signature), so the server must pick one of these. */
    { static const u8 sa[]={
        0x04,0x03,                       /* ecdsa_secp256r1_sha256           */
        0x05,0x03,                       /* ecdsa_secp384r1_sha384           */
        0x04,0x01 };                     /* rsa_pkcs1_sha256                 */
      int n=(int)sizeof(sa);
      h[q++]=0x00; h[q++]=0x0D; put16(h+q,(u32)(n+2)); q+=2; put16(h+q,(u32)n); q+=2;
      t_memcpy(h+q,sa,(u32)n); q+=n; }
    /* renegotiation_info (empty) — improves compatibility */
    h[q++]=0xFF; h[q++]=0x01; h[q++]=0x00; h[q++]=0x01; h[q++]=0x00;
    put16(h+ext_len_at,(u32)(q-(ext_len_at+2)));         /* fill extensions len */
    /* handshake header */
    int body=q-hs_start;
    h[0]=0x01; h[1]=(u8)(body>>16); h[2]=(u8)(body>>8); h[3]=(u8)body;
    sha256_update(&c->transcript,h,(u32)q);
    dbg("[tls] -> ClientHello\n");
    return send_plain_record(c->sock,22,h,q);
}

/* ── parse ServerHello / ServerKeyExchange ────────────────────────────────── */
static int parse_server_hello(tls_conn* c,const u8* b,int n){
    if(n<38) return -1;
    /* b[0..1] version, b[2..33] random, b[34] sid_len */
    t_memcpy(c->server_random,b+2,32);
    int sid=b[34];
    int off=35+sid;
    if(off+3>n) return -1;
    c->cipher=(u16)((b[off]<<8)|b[off+1]);
    dbghex("[tls] ServerHello cipher",b+off,2);
    if(c->cipher!=0xC02F && c->cipher!=0xC02B){ dbg("[tls] unexpected cipher\n"); return -1; }
    return 0;
}
/* Certificate message: 3-byte total len, then repeated (3-byte len || DER).
   Records pointers (aliasing s_hs) + lengths for post-flight verification.  */
static int parse_certificate(tls_conn* c,const u8* b,int n){
    if(n<3) return -1;
    int total=(b[0]<<16)|(b[1]<<8)|b[2];
    int off=3, end=3+total;
    if(end>n) return -1;
    c->cert_count=0;
    while(off+3<=end && c->cert_count<MAX_CERTS){
        int cl=(b[off]<<16)|(b[off+1]<<8)|b[off+2]; off+=3;
        if(cl<=0 || off+cl>end) break;
        c->certs[c->cert_count]=b+off; c->cert_lens[c->cert_count]=cl; c->cert_count++;
        off+=cl;
    }
    return c->cert_count>0 ? 0 : -1;
}

static int parse_ske(tls_conn* c,const u8* b,int n){
    if(n<4) return -1;
    if(b[0]!=0x03){ dbg("[tls] SKE not named_curve\n"); return -1; }   /* curve_type */
    int curve=(b[1]<<8)|b[2];
    int pklen=b[3];
    if(curve==0x001D){                                   /* x25519, 32-byte pub  */
        if(pklen!=32 || 4+pklen>n) return -1;
        c->curve=0x001D; t_memcpy(c->server_pub,b+4,32);
    }else if(curve==0x0017){                             /* secp256r1 04||X||Y   */
        if(pklen!=65 || 4+pklen>n || b[4]!=0x04) return -1;  /* len check BEFORE b[4] */
        c->curve=0x0017; t_memcpy(c->server_pub,b+5,64);
    }else{ dbg("[tls] SKE unknown curve\n"); return -1; }
    dbghex("[tls] server ECDHE pub",c->server_pub,c->curve==0x0017?64:32);
    /* capture the signed ServerECDHParams + the signature (TLS 1.2 format):
       params[4+pklen] || hash_alg(1) || sig_alg(1) || sig_len(2) || sig       */
    int so=4+pklen;
    if(so+4>n) return -1;
    c->ske_params=b; c->ske_params_len=so;
    c->ske_hash_alg=b[so]; c->ske_sig_alg=b[so+1];
    int slen=(b[so+2]<<8)|b[so+3];
    if(so+4+slen>n) return -1;
    c->ske_sig=b+so+4; c->ske_sig_len=slen;
    return 0;
}

/* ── full handshake ───────────────────────────────────────────────────────── */
static int tls_handshake(tls_conn* c,const char* host){
    sha256_init(&c->transcript);
    if(send_client_hello(c,host)<0){ dbg("[tls] send CH fail\n"); return -1; }

    /* read server flight until ServerHelloDone */
    int hs_len=0,parsed=0,done=0,got_sh=0,got_ske=0;
    while(!done){
        int type,plen;
        int r=read_record(c,&type,s_plain,&plen);
        if(r<=0){ dbg("[tls] flight read fail\n"); return -1; }
        if(type==21){ dbghex("[tls] ALERT",s_plain,plen<2?plen:2); return -1; }
        if(type!=22){ dbg("[tls] expected handshake\n"); return -1; }
        if(hs_len+plen>HS_MAX){ dbg("[tls] handshake too big\n"); return -1; }
        t_memcpy(s_hs+hs_len,s_plain,(u32)plen); hs_len+=plen;
        while(parsed+4<=hs_len){
            int mt=s_hs[parsed];
            int ml=(s_hs[parsed+1]<<16)|(s_hs[parsed+2]<<8)|s_hs[parsed+3];
            if(parsed+4+ml>hs_len) break;              /* need more data */
            const u8* body=s_hs+parsed+4;
            if(mt==2){ if(parse_server_hello(c,body,ml)<0)return -1; got_sh=1; }
            else if(mt==11){ if(parse_certificate(c,body,ml)<0)return -1; }
            else if(mt==12){ if(parse_ske(c,body,ml)<0)return -1; got_ske=1; }
            else if(mt==14){ done=1; }
            parsed+=4+ml;
            if(done) break;
        }
    }
    if(!got_sh||!got_ske){ dbg("[tls] missing SH/SKE\n"); return -1; }
    sha256_update(&c->transcript,s_hs,(u32)parsed);     /* server flight into transcript */

    /* ── authenticate the server: certificate chain + key-exchange signature ──
       (1) the chain must be trusted for `host` and time-valid; (2) the SKE must
       be signed by the leaf's key, binding the ephemeral ECDHE key to the cert.
       Either failure aborts the handshake with a certificate error (−3).      */
    if(c->cert_count<1){ set_cert_err("aucun certificat"); dbg("[tls] no cert\n"); return -3; }
    { int cc=cert_verify_chain(c->certs,c->cert_lens,c->cert_count,host,rtc_now());
      if(cc!=CERT_OK){ set_cert_err(cert_strerror(cc)); dbg("[tls] cert chain INVALID\n"); return -3; } }
    { u8 sd[32+32+70]; int sdl=64+c->ske_params_len;
      if(c->ske_params_len>70){ set_cert_err("parametres ECDHE trop longs"); return -3; }
      t_memcpy(sd,c->client_random,32);
      t_memcpy(sd+32,c->server_random,32);
      t_memcpy(sd+64,c->ske_params,(u32)c->ske_params_len);
      if(!cert_verify_signature(c->certs[0],c->cert_lens[0],c->ske_hash_alg,c->ske_sig_alg,
                                sd,sdl,c->ske_sig,c->ske_sig_len)){
          set_cert_err("signature ServerKeyExchange invalide"); dbg("[tls] SKE sig INVALID\n"); return -3; } }
    dbg("[tls] server authenticated (chain + SKE sig OK)\n");

    /* ECDHE: our keypair + shared secret (curve-dependent) */
    u8 priv[32]; tls_rand(priv,32); priv[31]|=1;         /* ensure nonzero scalar */
    u8 cke_pub[65]; int cke_pub_len; u8 pms[32];
    if(c->curve==0x0017){                                /* P-256 */
        u8 pub[64]; p256_scalarmult_base(pub,priv);
        if(!p256_ecdh(pms,priv,c->server_pub)){ dbg("[tls] p256 ecdh fail\n"); return -1; }
        cke_pub[0]=0x04; t_memcpy(cke_pub+1,pub,64); cke_pub_len=65;
    }else{                                               /* x25519 */
        u8 pub[32]; x25519_base(pub,priv); x25519(pms,priv,c->server_pub);
        t_memcpy(cke_pub,pub,32); cke_pub_len=32;
    }

    /* master_secret = PRF(pms,"master secret",client_random+server_random)[48] */
    u8 seed[64];
    t_memcpy(seed,c->client_random,32); t_memcpy(seed+32,c->server_random,32);
    tls12_prf(pms,32,"master secret",seed,64,c->master,48);
    /* key_block = PRF(master,"key expansion",server_random+client_random)[40] */
    t_memcpy(seed,c->server_random,32); t_memcpy(seed+32,c->client_random,32);
    u8 kb[40];
    tls12_prf(c->master,48,"key expansion",seed,64,kb,40);
    t_memcpy(c->client_key,kb,16);
    t_memcpy(c->server_key,kb+16,16);
    t_memcpy(c->client_iv,kb+32,4);
    t_memcpy(c->server_iv,kb+36,4);

    /* ClientKeyExchange: our ECDHE public value */
    { u8 cke[4+1+65];
      int body=1+cke_pub_len;
      cke[0]=0x10; cke[1]=(u8)(body>>16); cke[2]=(u8)(body>>8); cke[3]=(u8)body;
      cke[4]=(u8)cke_pub_len; t_memcpy(cke+5,cke_pub,(u32)cke_pub_len);
      sha256_update(&c->transcript,cke,(u32)(4+body));
      if(send_plain_record(c->sock,22,cke,4+body)<0) return -1;
      dbg("[tls] -> ClientKeyExchange\n"); }

    /* ChangeCipherSpec */
    { u8 ccs=0x01; if(send_plain_record(c->sock,20,&ccs,1)<0) return -1; }
    c->client_enc=1; c->client_seq=0;

    /* client Finished (encrypted): PRF(master,"client finished",H(transcript))[12] */
    { sha256_ctx snap=c->transcript; u8 hash[32]; sha256_final(&snap,hash);
      u8 vd[12]; tls12_prf(c->master,48,"client finished",hash,32,vd,12);
      u8 fin[16]; fin[0]=0x14; fin[1]=0; fin[2]=0; fin[3]=12; t_memcpy(fin+4,vd,12);
      sha256_update(&c->transcript,fin,16);             /* client Finished into transcript */
      if(send_enc_record(c,22,fin,16)<0) return -1;
      dbg("[tls] -> Finished (encrypted)\n"); }

    /* server ChangeCipherSpec + Finished */
    { int seen_ccs=0;
      for(int i=0;i<8;i++){
          int type,plen;
          int r=read_record(c,&type,s_plain,&plen);
          if(r<=0){ dbg("[tls] server finish read fail\n"); return -1; }
          if(type==20){ c->server_enc=1; c->server_seq=0; seen_ccs=1; continue; }
          if(type==21){ dbghex("[tls] server ALERT",s_plain,plen<2?plen:2); return -1; }
          if(type==22 && seen_ccs){
              /* server Finished — verify verify_data */
              sha256_ctx snap=c->transcript; u8 hash[32]; sha256_final(&snap,hash);
              u8 vd[12]; tls12_prf(c->master,48,"server finished",hash,32,vd,12);
              if(plen>=16){
                  int ok=1; for(int k=0;k<12;k++) if(s_plain[4+k]!=vd[k]) ok=0;
                  if(!ok){ dbg("[tls] server Finished MISMATCH\n"); return -1; }
                  dbg("[tls] handshake OK (Finished verified)\n");
                  return 0;
              }
              return -1;
          }
      }
      return -1; }
}

/* ── URL parse (https only) ───────────────────────────────────────────────── */
static int parse_https_url(const char* url,char* host,int hcap,int* port,char* path,int pcap){
    const char* p=url;
    if(!(p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]=='s'&&p[5]==':'&&p[6]=='/'&&p[7]=='/'))
        return 0;
    p+=8; int hi=0; *port=443;
    while(*p&&*p!='/'&&*p!=':'&&hi<hcap-1) host[hi++]=*p++;
    host[hi]='\0';
    if(*p==':'){ p++; int pn=0; while(*p>='0'&&*p<='9'){pn=pn*10+(*p-'0');p++;} if(pn>0&&pn<65536)*port=pn; }
    int pi=0;
    if(*p=='\0'){ path[pi++]='/'; }
    else { while(*p&&pi<pcap-1) path[pi++]=*p++; }
    path[pi]='\0';
    return 1;
}

static const char* find_hdr(const char* buf,int len,const char* name){
    int nl=t_strlen(name);
    for(int i=0;i+nl<len;i++){
        int m=1;
        for(int j=0;j<nl;j++){
            char a=buf[i+j],b=name[j];
            if(a>='A'&&a<='Z')a=(char)(a+32);
            if(b>='A'&&b<='Z')b=(char)(b+32);
            if(a!=b){m=0;break;}
        }
        if(m){ const char* v=buf+i+nl; while(*v==' '||*v==':')v++; return v; }
    }
    return 0;
}
static void cpstr(char* d,const char* s,int cap){int i=0;while(s[i]&&i<cap-1){d[i]=s[i];i++;}d[i]='\0';}

/* Does header `name` carry `tok` (case-insensitive) in its value line? */
static int hdr_has_token(const char* buf,int hlen,const char* name,const char* tok){
    const char* v=find_hdr(buf,hlen,name);
    if(!v) return 0;
    int tl=t_strlen(tok);
    for(int i=0; v[i] && v[i]!='\r' && v[i]!='\n'; i++){
        int m=1;
        for(int j=0;j<tl;j++){
            char a=v[i+j],b=tok[j];
            if(a>='A'&&a<='Z')a=(char)(a+32);
            if(b>='A'&&b<='Z')b=(char)(b+32);
            if(a!=b){m=0;break;}
        }
        if(m) return 1;
    }
    return 0;
}

/* Decode HTTP/1.1 chunked transfer-encoding in place. Returns decoded length.
   out <= in throughout, so an in-place rewrite is safe.                     */
static int dechunk(char* buf,int len){
    int in=0,out=0;
    while(in<len){
        unsigned int size=0; int any=0,digits=0;
        while(in<len){
            char ch=buf[in]; int d;
            if(ch>='0'&&ch<='9')d=ch-'0';
            else if(ch>='a'&&ch<='f')d=ch-'a'+10;
            else if(ch>='A'&&ch<='F')d=ch-'A'+10;
            else break;
            if(digits<7) size=size*16u+(unsigned)d;  /* cap 7 hex → no overflow */
            any=1; in++; digits++;
        }
        if(!any) break;                              /* malformed */
        while(in<len&&buf[in]!='\n') in++;           /* skip ext + CR */
        if(in<len) in++;                             /* skip LF */
        if(size==0) break;                           /* last chunk */
        unsigned int avail=(unsigned)(len-in);       /* len-in >= 0 here */
        if(size>avail) size=avail;                   /* overflow-safe clamp */
        for(unsigned int i=0;i<size;i++) buf[out++]=buf[in++];
        if(in<len&&buf[in]=='\r') in++;
        if(in<len&&buf[in]=='\n') in++;
    }
    return out;
}

/* ── https_get ────────────────────────────────────────────────────────────── */
int https_get(const char* url,char** out_body,int* out_len,int max_len,
              char* final_url,char* content_type){
    if(out_body)*out_body=0;
    if(out_len)*out_len=0;
    if(content_type)content_type[0]='\0';
    if(!net_ready()) return 0;

    char cur[512]; cpstr(cur,url,sizeof(cur));

    for(int redir=0;redir<6;redir++){
        char host[256],path[512]; int port;
        if(!parse_https_url(cur,host,sizeof(host),&port,path,sizeof(path))){
            /* redirect target may be plain http → delegate */
            if(cur[0]=='h'&&cur[1]=='t'&&cur[2]=='t'&&cur[3]=='p'&&cur[4]==':')
                return http_get(cur,out_body,out_len,max_len,final_url,content_type);
            return -1;
        }
        dbg("[tls] connect "); dbg(host); dbg("\n");
        u32 ip=net_dns_resolve(host);
        if(!ip){ dbg("[tls] DNS fail\n"); return 0; }
        int sock=tcp_connect(ip,(u16)port,8000);
        if(sock<0){ dbg("[tls] TCP fail\n"); return 0; }

        tls_conn c; t_memset(&c,0,sizeof(c)); c.sock=sock;
        { int hr=tls_handshake(&c,host);
          if(hr!=0){ tcp_close(sock); return hr==-3 ? -3 : -2; } }   /* -3 = cert invalid */

        /* GET request over an encrypted application-data record */
        { char req[900]; int rp=0;
          const char* g="GET "; for(int i=0;g[i];i++)req[rp++]=g[i];
          for(int i=0;path[i]&&rp<700;i++)req[rp++]=path[i];
          const char* m=" HTTP/1.1\r\nHost: "; for(int i=0;m[i];i++)req[rp++]=m[i];
          for(int i=0;host[i]&&rp<820;i++)req[rp++]=host[i];
          const char* t="\r\nUser-Agent: GregNet/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
          for(int i=0;t[i];i++)req[rp++]=t[i];
          if(send_enc_record(&c,23,(u8*)req,rp)<0){ tcp_close(sock); return -2; }
          dbg("[tls] -> GET (encrypted)\n"); }

        /* read encrypted application data until close */
        int cap=max_len+2048;
        char* raw=(char*)kmalloc((unsigned)cap);
        if(!raw){ tcp_close(sock); return 0; }
        int total=0;
        for(;;){
            int type,plen;
            int r=read_record(&c,&type,s_plain,&plen);
            if(r<=0) break;                            /* EOF / error */
            if(type==23){
                if(total+plen>cap-1) plen=cap-1-total;
                if(plen>0){ t_memcpy((u8*)raw+total,s_plain,(u32)plen); total+=plen; }
                if(total>=cap-1) break;
            }else if(type==21){ break; }                /* alert / close_notify */
            /* type==22 (NewSessionTicket) → ignore */
        }
        raw[total]='\0';
        tcp_close(sock);
        dbgint("[tls] response bytes",total);
        if(total<12){ kfree(raw); return 0; }

        /* parse status line */
        int status=0;
        { const char* sp=raw; while(*sp&&*sp!=' ')sp++; while(*sp==' ')sp++;
          for(int i=0;i<3&&sp[i]>='0'&&sp[i]<='9';i++) status=status*10+(sp[i]-'0'); }

        int body_off=-1;
        for(int i=0;i+3<total;i++){
            if(raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n'){body_off=i+4;break;}
            if(raw[i]=='\n'&&raw[i+1]=='\n'){body_off=i+2;break;}
        }
        int header_len=(body_off<0)?total:body_off;

        /* redirects */
        if(status==301||status==302||status==303||status==307||status==308){
            const char* loc=find_hdr(raw,header_len,"location");
            if(loc){
                char nu[512]; int k=0;
                while(loc[k]&&loc[k]!='\r'&&loc[k]!='\n'&&k<(int)sizeof(nu)-1){nu[k]=loc[k];k++;}
                nu[k]='\0'; kfree(raw);
                if(nu[0]=='h'&&nu[1]=='t'&&nu[2]=='t'&&nu[3]=='p'){ cpstr(cur,nu,sizeof(cur)); continue; }
                /* relative → same host */
                char rebuilt[512]; int w=0; const char* pre="https://";
                for(int i=0;pre[i];i++)rebuilt[w++]=pre[i];
                for(int i=0;host[i]&&w<400;i++)rebuilt[w++]=host[i];
                if(nu[0]!='/'&&w<500)rebuilt[w++]='/';
                for(int i=0;nu[i]&&w<510;i++)rebuilt[w++]=nu[i];
                rebuilt[w]='\0'; cpstr(cur,rebuilt,sizeof(cur)); continue;
            }
        }

        if(content_type){
            const char* ct=find_hdr(raw,header_len,"content-type");
            if(ct){int k=0;while(ct[k]&&ct[k]!='\r'&&ct[k]!='\n'&&ct[k]!=';'&&k<63){content_type[k]=ct[k];k++;}content_type[k]='\0';}
        }

        int blen=(body_off<0)?0:(total-body_off);
        if(body_off>=0 && hdr_has_token(raw,header_len,"transfer-encoding","chunked")){
            blen=dechunk(raw+body_off,blen);
            dbgint("[tls] dechunked bytes",blen);
        }
        if(blen>max_len)blen=max_len;
        char* body=(char*)kmalloc((unsigned)(blen+1));
        if(!body){ kfree(raw); return 0; }
        if(blen>0) t_memcpy(body,raw+body_off,(u32)blen);
        body[blen]='\0';
        kfree(raw);
        if(out_body)*out_body=body; else kfree(body);
        if(out_len)*out_len=blen;
        if(final_url) cpstr(final_url,cur,512);
        return status?status:200;
    }
    return 0;
}
