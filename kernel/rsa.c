/* rsa.c — RSA PKCS#1 v1.5 signature verification (SHA-256) for GregOS TLS.
   Bignum modexp via Montgomery (CIOS), supports moduli up to 4096-bit.
   Freestanding, no libc/libgcc. Verified against Python cryptography KATs.   */

#include "../include/crypto.h"

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned char      u8;

#define MAXK 128            /* limbs → up to 4096-bit modulus */

static void bn_zero(u32* a,int k){ for(int i=0;i<k;i++) a[i]=0; }
static void bn_copy(u32* r,const u32* a,int k){ for(int i=0;i<k;i++) r[i]=a[i]; }
/* a >= b ? (k-limb, little-endian) → 1 if a>=b else 0 */
static int bn_ge(const u32* a,const u32* b,int k){
    for(int i=k-1;i>=0;i--){ if(a[i]!=b[i]) return a[i]>b[i]; }
    return 1;
}
/* r = a - b (assumes a>=b) */
static void bn_sub(u32* r,const u32* a,const u32* b,int k){
    u64 br=0; for(int i=0;i<k;i++){ u64 s=(u64)a[i]-b[i]-br; r[i]=(u32)s; br=(s>>63)&1; }
}
/* a = 2a mod n */
static void bn_dbl_mod(u32* a,const u32* n,int k){
    u32 carry=0;
    for(int i=0;i<k;i++){ u32 nx=(a[i]<<1)|carry; carry=a[i]>>31; a[i]=nx; }
    if(carry || bn_ge(a,n,k)) bn_sub(a,a,n,k);
}
/* a → a*R mod n  (R = 2^(32k)); done by 32k modular doublings. */
static void bn_to_mont(u32* a,const u32* n,int k){
    for(int i=0;i<32*k;i++) bn_dbl_mod(a,n,k);
}
static u32 modinv32(u32 a){ u32 x=1; for(int i=0;i<5;i++) x=x*(2u-a*x); return x; } /* a^-1 mod 2^32 */

/* r = a*b*R^-1 mod n  (CIOS Montgomery, k limbs, n0 = -n^-1 mod 2^32) */
static void mont_mul(u32* r,const u32* a,const u32* b,const u32* n,u32 n0,int k){
    u32 t[MAXK+2]; for(int i=0;i<k+2;i++) t[i]=0;
    for(int i=0;i<k;i++){
        u64 C=0;
        for(int j=0;j<k;j++){ u64 s=(u64)a[j]*b[i]+t[j]+C; t[j]=(u32)s; C=s>>32; }
        u64 s=(u64)t[k]+C; t[k]=(u32)s; t[k+1]=(u32)(s>>32);
        u32 m=(u32)((u64)t[0]*n0);
        C=0;
        { u64 s0=(u64)m*n[0]+t[0]; C=s0>>32; }
        for(int j=1;j<k;j++){ u64 s1=(u64)m*n[j]+t[j]+C; t[j-1]=(u32)s1; C=s1>>32; }
        u64 s2=(u64)t[k]+C; t[k-1]=(u32)s2; t[k]=t[k+1]+(u32)(s2>>32);
    }
    if(t[k] || bn_ge(t,n,k)) bn_sub(r,t,n,k); else bn_copy(r,t,k);
}

/* out = base^e mod n.  base/n: k limbs. e: elen bytes big-endian. */
static void bn_modexp(u32* out,const u32* base,const u8* e,int elen,const u32* n,int k){
    u32 n0=(u32)(0u-modinv32(n[0]));
    u32 basem[MAXK], result[MAXK], one[MAXK];
    bn_copy(basem,base,k); bn_to_mont(basem,n,k);      /* base*R mod n */
    bn_zero(one,k); one[0]=1; bn_copy(result,one,k); bn_to_mont(result,n,k); /* R mod n = mont(1) */
    for(int bi=0;bi<elen;bi++)
        for(int bit=7;bit>=0;bit--){
            mont_mul(result,result,result,n,n0,k);
            if((e[bi]>>bit)&1) mont_mul(result,result,basem,n,n0,k);
        }
    mont_mul(out,result,one,n,n0,k);                   /* from Montgomery */
}

/* ── PKCS#1 v1.5 (SHA-256) signature verification ───────────────────────────
   n_be/nlen: modulus (big-endian). e_be/elen: public exponent. sig: nlen bytes.
   hash: 32-byte SHA-256 of the signed data. Returns 1 if valid.             */
static const u8 SHA256_DIGESTINFO[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

int rsa_pkcs1_sha256_verify(const unsigned char* n_be,int nlen,
                            const unsigned char* e_be,int elen,
                            const unsigned char* sig,int siglen,
                            const unsigned char hash[32]){
    if(nlen<64 || nlen>MAXK*4 || (nlen&3) || siglen!=nlen) return 0;
    if(elen<1 || elen>8) return 0;                     /* real RSA e is 1–3 bytes */
    int k=nlen/4;
    u32 n[MAXK], s[MAXK], m[MAXK];
    for(int i=0;i<k;i++){ const u8* q=n_be+(k-1-i)*4;   /* BE bytes → LE limbs */
        n[i]=((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]; }
    if(!(n[0]&1)) return 0;                             /* modulus must be odd */
    for(int i=0;i<k;i++){ const u8* q=sig+(k-1-i)*4;
        s[i]=((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]; }
    if(bn_ge(s,n,k)) return 0;                          /* sig must be < n */

    bn_modexp(m,s,e_be,elen,n,k);

    u8 em[MAXK*4];                                      /* m as big-endian, nlen bytes */
    for(int i=0;i<k;i++){ u8* q=em+(k-1-i)*4;
        q[0]=(u8)(m[i]>>24); q[1]=(u8)(m[i]>>16); q[2]=(u8)(m[i]>>8); q[3]=(u8)m[i]; }

    /* build expected EM = 00 01 FF..FF 00 DigestInfo || hash */
    int tlen=19+32;
    int pslen=nlen-tlen-3;
    if(pslen<8) return 0;
    u8 diff=0;
    diff |= em[0]^0x00; diff |= em[1]^0x01;
    for(int i=0;i<pslen;i++) diff|=em[2+i]^0xFF;
    diff |= em[2+pslen]^0x00;
    int off=3+pslen;
    for(int i=0;i<19;i++) diff|=em[off+i]^SHA256_DIGESTINFO[i];
    for(int i=0;i<32;i++) diff|=em[off+19+i]^hash[i];
    return diff==0;
}
