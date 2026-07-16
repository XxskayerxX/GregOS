/* crypto.c — GregOS crypto primitives for TLS 1.2 (freestanding, no libc/libgcc).
   SHA-256, HMAC-SHA256, TLS 1.2 PRF, AES-128-GCM, X25519.
   Correctness verified against KATs in scratchpad/crypto_test.c on the host.  */

#include "../include/crypto.h"

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ── tiny freestanding helpers (no libc) ─────────────────────────────────── */
static void cr_memcpy(void* d, const void* s, u32 n)
{ u8* dd=(u8*)d; const u8* ss=(const u8*)s; while (n--) *dd++ = *ss++; }
static void cr_memset(void* d, int v, u32 n)
{ u8* dd=(u8*)d; while (n--) *dd++ = (u8)v; }

/* ═══════════════════════════════════════════════════════════════════════════
   SHA-256
   ═══════════════════════════════════════════════════════════════════════════*/
static u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static const u32 K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_block(sha256_ctx* c, const u8* p)
{
    u32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((u32)p[i*4]<<24)|((u32)p[i*4+1]<<16)|((u32)p[i*4+2]<<8)|((u32)p[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        u32 s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15]>>3);
        u32 s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u32 a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; ++i) {
        u32 S1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + S1 + ch + K256[i] + w[i];
        u32 S0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        u32 maj = (a & b) ^ (a & cc) ^ (b & cc);
        u32 t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void sha256_init(sha256_ctx* c)
{
    c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u; c->h[3]=0xa54ff53au;
    c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu; c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
    c->bitlen = 0; c->idx = 0;
}

void sha256_update(sha256_ctx* c, const u8* data, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        c->buf[c->idx++] = data[i];
        if (c->idx == 64) { sha256_block(c, c->buf); c->idx = 0; }
    }
    c->bitlen += (u64)len * 8;
}

void sha256_final(sha256_ctx* c, u8 out[32])
{
    u64 bits = c->bitlen;
    u8 pad = 0x80;
    sha256_update(c, &pad, 1);
    u8 zero = 0;
    while (c->idx != 56) sha256_update(c, &zero, 1);
    u8 lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (u8)(bits >> (56 - 8*i));
    /* update would re-add to bitlen; write the length block manually */
    for (int i = 0; i < 8; ++i) c->buf[c->idx++] = lb[i];
    sha256_block(c, c->buf); c->idx = 0;
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (u8)(c->h[i] >> 24);
        out[i*4+1] = (u8)(c->h[i] >> 16);
        out[i*4+2] = (u8)(c->h[i] >> 8);
        out[i*4+3] = (u8)(c->h[i]);
    }
}

void sha256(const u8* data, u32 len, u8 out[32])
{
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SHA-384 (SHA-512 truncated) — certificate signatures often use SHA-384
   ═══════════════════════════════════════════════════════════════════════════*/
static u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }
static const u64 K512[80] = {
  0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,
  0x3956c25bf348b538ull,0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,
  0xd807aa98a3030242ull,0x12835b0145706fbeull,0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,
  0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,0xc19bf174cf692694ull,
  0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,
  0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,
  0x983e5152ee66dfabull,0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,
  0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,0x06ca6351e003826full,0x142929670a0e6e70ull,
  0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,0x53380d139d95b3dfull,
  0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
  0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,
  0xd192e819d6ef5218ull,0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,
  0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,
  0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,0x682e6ff3d6b2b8a3ull,
  0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,
  0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,
  0xca273eceea26619cull,0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,
  0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,0x113f9804bef90daeull,0x1b710b35131c471bull,
  0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,0x431d67c49c100d4cull,
  0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull
};
static const u64 IV384[8] = {
  0xcbbb9d5dc1059ed8ull,0x629a292a367cd507ull,0x9159015a3070dd17ull,0x152fecd8f70e5939ull,
  0x67332667ffc00b31ull,0x8eb44a8768581511ull,0xdb0c2e0d64f98fa7ull,0x47b5481dbefa4fa4ull
};
static void sha512_block(u64 h[8], const u8* p){
    u64 w[80];
    for (int i = 0; i < 16; ++i) { w[i]=0; for (int j=0;j<8;++j) w[i]=(w[i]<<8)|p[i*8+j]; }
    for (int i = 16; i < 80; ++i) {
        u64 s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15]>>7);
        u64 s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2]>>6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u64 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; ++i) {
        u64 S1 = ror64(e,14) ^ ror64(e,18) ^ ror64(e,41);
        u64 ch = (e & f) ^ ((~e) & g);
        u64 t1 = hh + S1 + ch + K512[i] + w[i];
        u64 S0 = ror64(a,28) ^ ror64(a,34) ^ ror64(a,39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
void sha384(const u8* data, u32 len, u8 out[48]){
    u64 h[8]; for (int i=0;i<8;++i) h[i]=IV384[i];
    u8 block[128]; u32 i=0;
    while (len - i >= 128) { sha512_block(h, data + i); i += 128; }
    u32 rem = len - i;
    for (u32 j=0;j<rem;++j) block[j]=data[i+j];
    block[rem]=0x80; u32 pad=rem+1;
    if (pad > 112) { while (pad<128) block[pad++]=0; sha512_block(h,block); pad=0; }
    while (pad<112) block[pad++]=0;
    u64 bits = (u64)len * 8;
    for (int j=0;j<8;++j) block[112+j]=0;                 /* high 64 bits of length */
    for (int j=0;j<8;++j) block[120+j]=(u8)(bits>>(56-8*j));
    sha512_block(h, block);
    for (int k=0;k<6;++k) for (int j=0;j<8;++j) out[k*8+j]=(u8)(h[k]>>(56-8*j));
}

/* ═══════════════════════════════════════════════════════════════════════════
   HMAC-SHA256
   ═══════════════════════════════════════════════════════════════════════════*/
void hmac_sha256(const u8* key, u32 klen, const u8* msg, u32 mlen, u8 out[32])
{
    u8 k[64], ipad[64], opad[64], inner[32];
    cr_memset(k, 0, 64);
    if (klen > 64) { sha256(key, klen, k); }        /* k = H(key), rest zero */
    else cr_memcpy(k, key, klen);
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TLS 1.2 PRF — P_SHA256(secret, label || seed)
   ═══════════════════════════════════════════════════════════════════════════*/
void tls12_prf(const u8* secret, u32 slen, const char* label,
               const u8* seed, u32 seedlen, u8* out, u32 outlen)
{
    /* build the PRF seed = label-bytes || seed */
    u8 fseed[256];
    u32 llen = 0; while (label[llen]) ++llen;
    if (llen + seedlen > sizeof(fseed)) return;      /* caller keeps it small */
    cr_memcpy(fseed, label, llen);
    cr_memcpy(fseed + llen, seed, seedlen);
    u32 fslen = llen + seedlen;

    /* A(0) = seed; A(i) = HMAC(secret, A(i-1)) */
    u8 a[32];
    hmac_sha256(secret, slen, fseed, fslen, a);      /* A(1) */

    u32 done = 0;
    u8 buf[32 + 256];
    while (done < outlen) {
        /* HMAC(secret, A(i) || seed) */
        cr_memcpy(buf, a, 32);
        cr_memcpy(buf + 32, fseed, fslen);
        u8 block[32];
        hmac_sha256(secret, slen, buf, 32 + fslen, block);
        u32 take = outlen - done; if (take > 32) take = 32;
        cr_memcpy(out + done, block, take);
        done += take;
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, slen, a, 32, a);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   AES-128 (encrypt only — enough for GCM/CTR)
   ═══════════════════════════════════════════════════════════════════════════*/
static const u8 SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const u8 RCON[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static void aes128_key_expand(const u8 key[16], u8 rk[176])
{
    cr_memcpy(rk, key, 16);
    int bytes = 16;
    u8 t[4];
    int rcon_i = 0;
    while (bytes < 176) {
        for (int i = 0; i < 4; ++i) t[i] = rk[bytes - 4 + i];
        if (bytes % 16 == 0) {
            u8 tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;   /* RotWord */
            for (int i = 0; i < 4; ++i) t[i] = SBOX[t[i]];              /* SubWord */
            t[0] ^= RCON[rcon_i++];
        }
        for (int i = 0; i < 4; ++i) { rk[bytes] = rk[bytes - 16] ^ t[i]; ++bytes; }
    }
}

static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16])
{
    u8 s[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = SBOX[s[i]];            /* SubBytes */
        /* ShiftRows (state is column-major: s[r + 4c]) */
        u8 t[16];
        t[0]=s[0];  t[4]=s[4];  t[8]=s[8];   t[12]=s[12];
        t[1]=s[5];  t[5]=s[9];  t[9]=s[13];  t[13]=s[1];
        t[2]=s[10]; t[6]=s[14]; t[10]=s[2];  t[14]=s[6];
        t[3]=s[15]; t[7]=s[3];  t[11]=s[7];  t[15]=s[11];
        for (int i = 0; i < 16; ++i) s[i] = t[i];
        if (round != 10) {                                        /* MixColumns */
            for (int c = 0; c < 4; ++c) {
                u8* col = s + 4*c;
                u8 a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0] = (u8)(xtime(a0) ^ (xtime(a1)^a1) ^ a2 ^ a3);
                col[1] = (u8)(a0 ^ xtime(a1) ^ (xtime(a2)^a2) ^ a3);
                col[2] = (u8)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3)^a3));
                col[3] = (u8)((xtime(a0)^a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; ++i) s[i] ^= rk[round*16 + i];    /* AddRoundKey */
    }
    for (int i = 0; i < 16; ++i) out[i] = s[i];
}

/* ═══════════════════════════════════════════════════════════════════════════
   GHASH + AES-128-GCM
   ═══════════════════════════════════════════════════════════════════════════*/
static u64 be64(const u8* p)
{ return ((u64)p[0]<<56)|((u64)p[1]<<48)|((u64)p[2]<<40)|((u64)p[3]<<32)
       | ((u64)p[4]<<24)|((u64)p[5]<<16)|((u64)p[6]<<8)|((u64)p[7]); }
static void put_be64(u8* p, u64 v)
{ for (int i = 0; i < 8; ++i) p[i] = (u8)(v >> (56 - 8*i)); }

/* Y = Y · H over GF(2^128), bit 0 = MSB of byte 0 (SP800-38D). */
static void ghash_mul(u8 Y[16], const u8 H[16])
{
    u64 zh=0, zl=0, vh=be64(H), vl=be64(H+8);
    for (int i = 0; i < 128; ++i) {
        u8 bit = (Y[i>>3] >> (7 - (i & 7))) & 1;
        if (bit) { zh ^= vh; zl ^= vl; }
        u64 carry = vl & 1ULL;
        vl = (vl >> 1) | (vh << 63);
        vh = vh >> 1;
        if (carry) vh ^= 0xe100000000000000ULL;
    }
    put_be64(Y, zh); put_be64(Y+8, zl);
}

static void ghash_blocks(u8 Y[16], const u8 H[16], const u8* data, u32 len)
{
    u32 i = 0;
    while (i + 16 <= len) {
        for (int j = 0; j < 16; ++j) Y[j] ^= data[i+j];
        ghash_mul(Y, H);
        i += 16;
    }
    if (i < len) {                                   /* zero-padded remainder */
        u8 blk[16]; cr_memset(blk, 0, 16);
        cr_memcpy(blk, data + i, len - i);
        for (int j = 0; j < 16; ++j) Y[j] ^= blk[j];
        ghash_mul(Y, H);
    }
}

static void gcm_incr(u8 ctr[16])
{ for (int i = 15; i >= 12; --i) { if (++ctr[i]) break; } }

static void gcm_core(const u8 key[16], const u8 iv[12],
                     const u8* aad, u32 aadlen,
                     const u8* in, u32 inlen, u8* out,
                     u8 tag[16], int encrypt)
{
    u8 rk[176]; aes128_key_expand(key, rk);
    u8 H[16], zero[16]; cr_memset(zero, 0, 16);
    aes128_encrypt_block(rk, zero, H);

    u8 J0[16]; cr_memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;

    /* CTR encryption from inc32(J0) */
    u8 ctr[16]; cr_memcpy(ctr, J0, 16);
    u32 i = 0;
    while (i < inlen) {
        gcm_incr(ctr);
        u8 ks[16]; aes128_encrypt_block(rk, ctr, ks);
        u32 n = inlen - i; if (n > 16) n = 16;
        for (u32 j = 0; j < n; ++j) out[i+j] = in[i+j] ^ ks[j];
        i += n;
    }

    /* GHASH over AAD || ciphertext || len-block; ciphertext is `out` on
       encrypt, `in` on decrypt.                                            */
    const u8* ct = encrypt ? out : in;
    u8 Y[16]; cr_memset(Y, 0, 16);
    ghash_blocks(Y, H, aad, aadlen);
    ghash_blocks(Y, H, ct, inlen);
    u8 lenblk[16];
    put_be64(lenblk, (u64)aadlen * 8);
    put_be64(lenblk + 8, (u64)inlen * 8);
    for (int j = 0; j < 16; ++j) Y[j] ^= lenblk[j];
    ghash_mul(Y, H);

    u8 ej0[16]; aes128_encrypt_block(rk, J0, ej0);
    for (int j = 0; j < 16; ++j) tag[j] = Y[j] ^ ej0[j];
}

void aes128_gcm_encrypt(const u8 key[16], const u8 iv[12],
                        const u8* aad, u32 aadlen,
                        const u8* pt, u32 ptlen, u8* ct, u8 tag[16])
{
    gcm_core(key, iv, aad, aadlen, pt, ptlen, ct, tag, 1);
}

int aes128_gcm_decrypt(const u8 key[16], const u8 iv[12],
                       const u8* aad, u32 aadlen,
                       const u8* ct, u32 ctlen, const u8 tag[16], u8* pt)
{
    u8 want[16];
    gcm_core(key, iv, aad, aadlen, ct, ctlen, pt, want, 0);
    u8 diff = 0;
    for (int j = 0; j < 16; ++j) diff |= (u8)(want[j] ^ tag[j]);
    return diff == 0 ? 1 : 0;                        /* constant-time compare */
}

/* ═══════════════════════════════════════════════════════════════════════════
   X25519 — TweetNaCl-derived (public domain). 16×16-bit limbs, no 64-bit mul
   on operands wider than 16 bits → GCC inlines everything (no libgcc).
   ═══════════════════════════════════════════════════════════════════════════*/
typedef long long i64;
typedef i64 gf[16];

static const gf X_121665 = {0xDB41, 1};

static void car25519(gf o)
{
    i64 c;
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i+1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    i64 t, c = ~(b - 1);
    for (int i = 0; i < 16; ++i) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(u8* o, const gf n)
{
    int b;
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) { o[2*i] = t[i] & 0xff; o[2*i+1] = t[i] >> 8; }
}

static void unpack25519(gf o, const u8* n)
{
    for (int i = 0; i < 16; ++i) o[i] = n[2*i] + ((i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { for (int i=0;i<16;++i) o[i]=a[i]+b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i=0;i<16;++i) o[i]=a[i]-b[i]; }

static void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
    u8 z[32];
    i64 r;
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; ++i) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a); M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, X_121665); A(a, a, d); M(c, c, a); M(a, d, f);
        M(d, b, x); S(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    /* q = a · inv(c)  (TweetNaCl finalization) */
    inv25519(c, c);
    M(a, a, c);
    pack25519(out, a);
}

void x25519_base(u8 out[32], const u8 scalar[32])
{
    u8 base[32]; cr_memset(base, 0, 32); base[0] = 9;
    x25519(out, scalar, base);
}
