/* p384.c — NIST P-384 (secp384r1) ECDSA verification for GregOS TLS.
   Structurally identical to p256.c but with 12-limb field elements and the
   P-384 domain parameters; points in Jacobian coords (a=-3), Montgomery (CIOS).
   Freestanding, no libc/libgcc. All I/O is big-endian (X.509 / TLS format).
   Needed because higher CA certificates use P-384 keys — with this the chain
   verifies to the root. Verified against Python `cryptography` SECP384R1 KATs. */

#include "../include/crypto.h"

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned char      u8;
typedef u32 fe[12];                /* field element: 12 little-endian limbs   */

/* ── P-384 constants (little-endian 32-bit limbs) ────────────────────────── */
static const u32 P[12]  = { 0xffffffffu,0x00000000u,0x00000000u,0xffffffffu,0xfffffffeu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu };
static const u32 R2[12] = { 0x00000001u,0xfffffffeu,0x00000000u,0x00000002u,0x00000000u,0xfffffffeu,0x00000000u,0x00000002u,0x00000001u,0x00000000u,0x00000000u,0x00000000u };
static const u32 B[12]  = { 0xd3ec2aefu,0x2a85c8edu,0x8a2ed19du,0xc656398du,0x5013875au,0x0314088fu,0xfe814112u,0x181d9c6eu,0xe3f82d19u,0x988e056bu,0xe23ee7e4u,0xb3312fa7u };
static const u32 GX[12] = { 0x72760ab7u,0x3a545e38u,0xbf55296cu,0x5502f25du,0x82542a38u,0x59f741e0u,0x8ba79b98u,0x6e1d3b62u,0xf320ad74u,0x8eb1c71eu,0xbe8b0537u,0xaa87ca22u };
static const u32 GY[12] = { 0x90ea0e5fu,0x7a431d7cu,0x1d7e819du,0x0a60b1ceu,0xb5f0b8c0u,0xe9da3113u,0x289a147cu,0xf8f41dbdu,0x9292dc29u,0x5d9e98bfu,0x96262c6fu,0x3617de4au };
#define N0 1u                       /* -p^-1 mod 2^32 */

static void fe_copy(fe r,const fe a){ for(int i=0;i<12;i++) r[i]=a[i]; }
static void fe_set0(fe r){ for(int i=0;i<12;i++) r[i]=0; }
static int  fe_iszero(const fe a){ u32 x=0; for(int i=0;i<12;i++) x|=a[i]; return x==0; }
static int  fe_eq(const fe a,const fe b){ u32 x=0; for(int i=0;i<12;i++) x|=a[i]^b[i]; return x==0; }
static void fe_cmov(fe r,const fe a,u32 mask){ for(int i=0;i<12;i++) r[i]=(r[i]&~mask)|(a[i]&mask); }

/* r = a + b mod p  (a,b < p) */
static void fe_add(fe r,const fe a,const fe b){
    u64 c=0; u32 t[12];
    for(int i=0;i<12;i++){ u64 s=(u64)a[i]+b[i]+c; t[i]=(u32)s; c=s>>32; }
    u64 br=0; u32 u[12];
    for(int i=0;i<12;i++){ u64 s=(u64)t[i]-P[i]-br; u[i]=(u32)s; br=(s>>63)&1; }
    u32 mask=(c!=0||br==0)?0xffffffffu:0u;    /* sum>=p → use (sum-p) */
    for(int i=0;i<12;i++) r[i]=(u[i]&mask)|(t[i]&~mask);
}
/* r = a - b mod p */
static void fe_sub(fe r,const fe a,const fe b){
    u64 br=0; u32 t[12];
    for(int i=0;i<12;i++){ u64 s=(u64)a[i]-b[i]-br; t[i]=(u32)s; br=(s>>63)&1; }
    u64 c=0; u32 u[12];
    for(int i=0;i<12;i++){ u64 s=(u64)t[i]+P[i]+c; u[i]=(u32)s; c=s>>32; }
    u32 mask=br?0xffffffffu:0u;                /* a<b → add p */
    for(int i=0;i<12;i++) r[i]=(u[i]&mask)|(t[i]&~mask);
}

/* Montgomery multiply: r = a*b*R^-1 mod p (CIOS). */
static void mont_mul(fe r,const fe a,const fe b){
    u32 t[14]; for(int i=0;i<14;i++) t[i]=0;
    for(int i=0;i<12;i++){
        u64 C=0;
        for(int j=0;j<12;j++){ u64 s=(u64)a[j]*b[i]+t[j]+C; t[j]=(u32)s; C=s>>32; }
        u64 s=(u64)t[12]+C; t[12]=(u32)s; t[13]=(u32)(s>>32);
        u32 m=(u32)((u64)t[0]*N0);
        C=0;
        { u64 s0=(u64)m*P[0]+t[0]; C=s0>>32; }     /* t[0] discarded (becomes 0) */
        for(int j=1;j<12;j++){ u64 s1=(u64)m*P[j]+t[j]+C; t[j-1]=(u32)s1; C=s1>>32; }
        u64 s2=(u64)t[12]+C; t[11]=(u32)s2; t[12]=t[13]+(u32)(s2>>32);
    }
    /* final conditional subtract p */
    u64 br=0; u32 u[12];
    for(int i=0;i<12;i++){ u64 s=(u64)t[i]-P[i]-br; u[i]=(u32)s; br=(s>>63)&1; }
    u32 mask=(t[12]!=0||br==0)?0xffffffffu:0u;
    for(int i=0;i<12;i++) r[i]=(u[i]&mask)|(t[i]&~mask);
}
static void fe_sqr(fe r,const fe a){ mont_mul(r,a,a); }
static void to_mont(fe r,const fe a){ mont_mul(r,a,R2); }
static void from_mont(fe r,const fe a){ fe one; fe_set0(one); one[0]=1; mont_mul(r,a,one); }

/* r = a^-1 mod p, via Fermat a^(p-2). a is in Montgomery form; r in Mont. */
static void fe_inv(fe r,const fe a){
    fe pm2; for(int i=0;i<12;i++) pm2[i]=P[i]; pm2[0]-=2;   /* p-2 (no borrow) */
    fe res; fe_set0(res); to_mont(res,(fe){1}); /* Mont one = R mod p */
    for(int i=383;i>=0;i--){
        fe_sqr(res,res);
        if((pm2[i>>5]>>(i&31))&1) mont_mul(res,res,a);
    }
    fe_copy(r,res);
}

/* ── Jacobian point arithmetic (all coords in Montgomery form) ───────────── */
typedef struct { fe X,Y,Z; } jac;

static void jac_dbl(jac* o,const jac* p){
    if(fe_iszero(p->Z)){ fe_copy(o->X,p->X); fe_copy(o->Y,p->Y); fe_set0(o->Z); return; }
    fe delta,gamma,beta,alpha,t0,t1,X3,Y3,Z3;
    fe_sqr(delta,p->Z);                 /* Z1^2 */
    fe_sqr(gamma,p->Y);                 /* Y1^2 */
    mont_mul(beta,p->X,gamma);          /* X1*gamma */
    fe_sub(t0,p->X,delta); fe_add(t1,p->X,delta); mont_mul(t0,t0,t1);
    fe_add(alpha,t0,t0); fe_add(alpha,alpha,t0);        /* 3(X1-δ)(X1+δ) */
    fe_sqr(X3,alpha);
    fe_add(t0,beta,beta); fe_add(t0,t0,t0); fe_add(t1,t0,t0); /* t1 = 8β, t0 = 4β */
    fe_sub(X3,X3,t1);
    fe_add(Z3,p->Y,p->Z); fe_sqr(Z3,Z3); fe_sub(Z3,Z3,gamma); fe_sub(Z3,Z3,delta);
    fe_sub(Y3,t0,X3); mont_mul(Y3,alpha,Y3);
    fe_sqr(t0,gamma); fe_add(t0,t0,t0); fe_add(t0,t0,t0); fe_add(t0,t0,t0); /* 8γ^2 */
    fe_sub(Y3,Y3,t0);
    fe_copy(o->X,X3); fe_copy(o->Y,Y3); fe_copy(o->Z,Z3);
}

static void jac_add(jac* o,const jac* p,const jac* q){
    if(fe_iszero(p->Z)){ *o=*q; return; }
    if(fe_iszero(q->Z)){ *o=*p; return; }
    fe Z1Z1,Z2Z2,U1,U2,S1,S2,H,I,J,r,V,t0,t1,X3,Y3,Z3;
    fe_sqr(Z1Z1,p->Z); fe_sqr(Z2Z2,q->Z);
    mont_mul(U1,p->X,Z2Z2); mont_mul(U2,q->X,Z1Z1);
    mont_mul(S1,p->Y,q->Z); mont_mul(S1,S1,Z2Z2);
    mont_mul(S2,q->Y,p->Z); mont_mul(S2,S2,Z1Z1);
    fe_sub(H,U2,U1);
    fe_sub(r,S2,S1);
    if(fe_iszero(H)){
        if(fe_iszero(r)){ jac_dbl(o,p); return; }       /* p == q */
        fe_set0(o->Z); fe_copy(o->X,p->X); fe_copy(o->Y,p->Y); return; /* opposite → ∞ */
    }
    fe_add(t0,H,H); fe_sqr(I,t0);                        /* I=(2H)^2 */
    mont_mul(J,H,I);
    fe_add(r,r,r);                                       /* r=2(S2-S1) */
    mont_mul(V,U1,I);
    fe_sqr(X3,r); fe_sub(X3,X3,J); fe_add(t0,V,V); fe_sub(X3,X3,t0);
    fe_sub(t0,V,X3); mont_mul(Y3,r,t0);
    mont_mul(t1,S1,J); fe_add(t1,t1,t1); fe_sub(Y3,Y3,t1);
    fe_add(Z3,p->Z,q->Z); fe_sqr(Z3,Z3); fe_sub(Z3,Z3,Z1Z1); fe_sub(Z3,Z3,Z2Z2); mont_mul(Z3,Z3,H);
    fe_copy(o->X,X3); fe_copy(o->Y,Y3); fe_copy(o->Z,Z3);
}

/* out = scalar (48-byte BE) · P, double-and-add MSB-first. */
static void scalar_mult(jac* out,const u8 scalar[48],const jac* Pt){
    jac R; fe_set0(R.X); fe_set0(R.Y); fe_set0(R.Z);   /* ∞ */
    for(int i=0;i<384;i++){
        jac D; jac_dbl(&D,&R); R=D;
        u32 bit=(scalar[i>>3]>>(7-(i&7)))&1;
        jac T; jac_add(&T,&R,Pt);
        u32 mask=(u32)(0-bit);
        fe_cmov(R.X,T.X,mask); fe_cmov(R.Y,T.Y,mask); fe_cmov(R.Z,T.Z,mask);
    }
    *out=R;
}

/* ── byte <-> fe (big-endian, wire format) ───────────────────────────────── */
static void be_to_fe(fe r,const u8* b){
    for(int i=0;i<12;i++){ const u8* q=b+(11-i)*4;
        r[i]=((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]; }
}
static void fe_to_be(u8* b,const fe a){
    for(int i=0;i<12;i++){ u8* q=b+(11-i)*4;
        q[0]=(u8)(a[i]>>24); q[1]=(u8)(a[i]>>16); q[2]=(u8)(a[i]>>8); q[3]=(u8)a[i]; }
}

/* Jacobian → affine x, as 48-byte BE (from Mont form). */
static void jac_affine_x(u8 out[48],const jac* p){
    fe zi,zi2,x;
    fe_inv(zi,p->Z); fe_sqr(zi2,zi); mont_mul(x,p->X,zi2);
    fe xn; from_mont(xn,x); fe_to_be(out,xn);
}

/* Is (x,y) on the curve y^2 = x^3 - 3x + b ? (Mont-form inputs) */
static int on_curve(const fe xm,const fe ym){
    fe y2,x3,t,bx,bm;
    fe_sqr(y2,ym);
    fe_sqr(x3,xm); mont_mul(x3,x3,xm);                  /* x^3 */
    fe_add(t,xm,xm); fe_add(t,t,xm);                    /* 3x */
    fe_sub(x3,x3,t);                                    /* x^3-3x */
    to_mont(bm,B); fe_add(bx,x3,bm);                    /* + b */
    return fe_eq(y2,bx);
}

/* ── arithmetic mod the group order n ─────────────────────────────────────── */
static const u32 N[12]    = { 0xccc52973u,0xecec196au,0x48b0a77au,0x581a0db2u,0xf4372ddfu,0xc7634d81u,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu };
static const u32 N_R2[12] = { 0x19b409a9u,0x2d319b24u,0xdf1aa419u,0xff3d81e5u,0xfcb82947u,0xbc3e483au,0x4aab1cc5u,0xd40d4917u,0x28266895u,0x3fb05b7au,0x2b39bf21u,0x0c84ee01u };
#define N_N0 0xe88fdc45u

static int  fe_lt(const fe a,const fe b){ u64 br=0; for(int i=0;i<12;i++){ u64 s=(u64)a[i]-b[i]-br; br=(s>>63)&1; } return (int)br; }
static void fe_sub_raw(fe r,const fe a,const fe b){ u64 br=0; for(int i=0;i<12;i++){ u64 s=(u64)a[i]-b[i]-br; r[i]=(u32)s; br=(s>>63)&1; } }

static void n_mul(fe r,const fe a,const fe b){
    u32 t[14]; for(int i=0;i<14;i++) t[i]=0;
    for(int i=0;i<12;i++){
        u64 C=0;
        for(int j=0;j<12;j++){ u64 s=(u64)a[j]*b[i]+t[j]+C; t[j]=(u32)s; C=s>>32; }
        u64 s=(u64)t[12]+C; t[12]=(u32)s; t[13]=(u32)(s>>32);
        u32 m=(u32)((u64)t[0]*N_N0);
        C=0;
        { u64 s0=(u64)m*N[0]+t[0]; C=s0>>32; }
        for(int j=1;j<12;j++){ u64 s1=(u64)m*N[j]+t[j]+C; t[j-1]=(u32)s1; C=s1>>32; }
        u64 s2=(u64)t[12]+C; t[11]=(u32)s2; t[12]=t[13]+(u32)(s2>>32);
    }
    u64 br=0; u32 u[12];
    for(int i=0;i<12;i++){ u64 s=(u64)t[i]-N[i]-br; u[i]=(u32)s; br=(s>>63)&1; }
    u32 mask=(t[12]!=0||br==0)?0xffffffffu:0u;
    for(int i=0;i<12;i++) r[i]=(u[i]&mask)|(t[i]&~mask);
}
static void to_mont_n(fe r,const fe a){ n_mul(r,a,N_R2); }
static void from_mont_n(fe r,const fe a){ fe one; fe_set0(one); one[0]=1; n_mul(r,a,one); }
static void n_inv(fe r,const fe a){                    /* a^(n-2) mod n; a,r normal */
    fe nm2; for(int i=0;i<12;i++) nm2[i]=N[i]; nm2[0]-=2;
    fe am; to_mont_n(am,a);
    fe res; to_mont_n(res,(fe){1});                    /* Mont one = R mod n */
    for(int i=383;i>=0;i--){ n_mul(res,res,res); if((nm2[i>>5]>>(i&31))&1) n_mul(res,res,am); }
    from_mont_n(r,res);
}

/* ── public API ──────────────────────────────────────────────────────────── */

/* Verify ECDSA-P384 signature (r,s big-endian 48B) over `hash` (`hashlen` bytes,
   32 for SHA-256 or 48 for SHA-384) against a 96-byte public key (X||Y BE).
   Returns 1 if valid, 0 otherwise. */
int p384_ecdsa_verify(const u8* hash,int hashlen,const u8 r_be[48],const u8 s_be[48],const u8 pub[96]){
    fe r,s; be_to_fe(r,r_be); be_to_fe(s,s_be);
    if(fe_iszero(r)||fe_iszero(s)) return 0;
    if(!fe_lt(r,N)||!fe_lt(s,N)) return 0;              /* r,s in [1,n-1] */
    fe px,py,pxm,pym; be_to_fe(px,pub); be_to_fe(py,pub+48);
    if(!fe_lt(px,P)||!fe_lt(py,P)) return 0;
    to_mont(pxm,px); to_mont(pym,py);
    if(fe_iszero(pxm)&&fe_iszero(pym)) return 0;        /* infinity */
    if(!on_curve(pxm,pym)) return 0;
    /* z = leftmost 384 bits of hash, right-aligned into 48 bytes (hashlen<=48) */
    u8 zb[48]; for(int i=0;i<48;i++) zb[i]=0;
    if(hashlen>48) hashlen=48;
    for(int i=0;i<hashlen;i++) zb[48-hashlen+i]=hash[i];
    fe z; be_to_fe(z,zb); if(!fe_lt(z,N)) fe_sub_raw(z,z,N);   /* z mod n */
    fe w; n_inv(w,s);                                   /* w = s^-1 mod n (normal) */
    fe u1,u2,zm,rm; to_mont_n(zm,z); to_mont_n(rm,r);
    n_mul(u1,zm,w); n_mul(u2,rm,w);                     /* u1=z*w, u2=r*w mod n */
    u8 u1b[48],u2b[48]; fe_to_be(u1b,u1); fe_to_be(u2b,u2);
    jac G; to_mont(G.X,GX); to_mont(G.Y,GY); to_mont(G.Z,(fe){1});
    jac Q; fe_copy(Q.X,pxm); fe_copy(Q.Y,pym); to_mont(Q.Z,(fe){1});
    jac A,Bp,S; scalar_mult(&A,u1b,&G); scalar_mult(&Bp,u2b,&Q); jac_add(&S,&A,&Bp);
    if(fe_iszero(S.Z)) return 0;
    u8 xb[48]; jac_affine_x(xb,&S);
    fe x1; be_to_fe(x1,xb); if(!fe_lt(x1,N)) fe_sub_raw(x1,x1,N);
    return fe_eq(x1,r);
}

#ifdef P384_TEST
/* KAT hooks (host only) — normal-form 48B BE in/out. */
void p384_test_fmul(u8 o[48],const u8 a[48],const u8 b[48]){
    fe x,y,am,r; be_to_fe(x,a); be_to_fe(y,b); to_mont(am,x); mont_mul(r,am,y); fe_to_be(o,r);
}
void p384_test_finv(u8 o[48],const u8 a[48]){ fe x,am,r,rn; be_to_fe(x,a); to_mont(am,x); fe_inv(r,am); from_mont(rn,r); fe_to_be(o,rn); }
void p384_test_ninv(u8 o[48],const u8 a[48]){ fe x,r; be_to_fe(x,a); n_inv(r,x); fe_to_be(o,r); }
/* pub(96B BE X||Y) = scalar(48B BE) · G  — exercises scalar_mult + jac ops */
void p384_test_basemul(u8 out[96],const u8 scalar[48]){
    jac G; to_mont(G.X,GX); to_mont(G.Y,GY); to_mont(G.Z,(fe){1});
    jac Q; scalar_mult(&Q,scalar,&G);
    fe zi,zi2,zi3,x,y,xn,yn;
    fe_inv(zi,Q.Z); fe_sqr(zi2,zi); mont_mul(zi3,zi2,zi);
    mont_mul(x,Q.X,zi2); mont_mul(y,Q.Y,zi3);
    from_mont(xn,x); from_mont(yn,y);
    fe_to_be(out,xn); fe_to_be(out+48,yn);
}
#endif
