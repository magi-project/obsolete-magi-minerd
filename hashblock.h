// Copyright (c) 2014 The Mini-Blockchain Project

#ifndef HASHBLOCK_H
#define HASHBLOCK_H

#include "uint256.h"
#include "util.h"

#include "hash/sph_sha2.h"
#include "hash/sph_keccak.h" //sha3
#include "hash/sph_haval.h"
#include "hash/sph_tiger.h"
#include "hash/sph_whirlpool.h"
#include "hash/sph_ripemd.h"

#ifndef QT_NO_DEBUG
#include <string>
#endif

#ifdef GLOBALDEFINED
#define GLOBAL
#else
#define GLOBAL extern
#endif

extern "C"{
    extern double __spectral_weight_m_MOD_sw(int *, int *);
}

GLOBAL sph_sha256_context     z_sha256;
GLOBAL sph_sha512_context     z_sha512;
GLOBAL sph_keccak512_context  z_keccak;
GLOBAL sph_whirlpool_context  z_whirlpool;
GLOBAL sph_haval256_5_context z_haval;
GLOBAL sph_tiger_context      z_tiger;
GLOBAL sph_ripemd160_context  z_ripemd;



#define fillz() do { \
    sph_sha512_init(&z_sha512); \
    sph_sha256_init(&z_sha256); \
    sph_keccak512_init(&z_keccak); \
    sph_whirlpool_init(&z_whirlpool); \
    sph_haval256_5_init(&z_haval); \
    sph_tiger_init(&z_tiger); \
    sph_ripemd160_init(&z_ripemd); \
} while (0) 

#define ZSHA256 (memcpy(&ctx_sha256, &z_sha256, sizeof(z_sha256)))
#define ZSHA512 (memcpy(&ctx_sha512, &z_sha512, sizeof(z_sha512)))
#define ZKECCAK (memcpy(&ctx_keccak, &z_keccak, sizeof(z_keccak)))
#define ZWHIRLPOOL (memcpy(&ctx_whirlpool, &z_whirlpool, sizeof(z_whirlpool)))
#define ZHAVAL (memcpy(&ctx_haval, &z_haval, sizeof(z_haval)))
#define ZTIGER (memcpy(&ctx_tiger, &z_tiger, sizeof(z_tiger)))
#define ZRIPEMD (memcpy(&ctx_ripemd, &z_ripemd, sizeof(z_ripemd)))

struct hash_context {
    sph_sha256_context       ctx_sha256;
    sph_sha512_context       ctx_sha512;
    sph_keccak512_context    ctx_keccak;
    sph_whirlpool_context    ctx_whirlpool;
    sph_haval256_5_context   ctx_haval;
    sph_tiger_context        ctx_tiger;
    sph_ripemd160_context    ctx_ripemd;
    mpz_t bns[8];
    mpz_t product;
    char data[4096];
};

void HashInit(hash_context &h){
    for(int i=0; i < 8; i++){
	mpz_init(h.bns[i]);
    }
    mpz_init(h.product);
}


#define NM7M 5
template<typename T1>
inline uint256 hash_M7M(hash_context &h, const T1 pbegin, const T1 pend, const unsigned int nnNonce)
{
    static unsigned char pblank[1];
    int bytes;
    unsigned int nnNonce1 = nnNonce/2;

    uint512 hash[7];
    uint256 finalhash;
    for(int i=0; i < 7; i++)
	hash[i] = 0;

    const void* ptr = (pbegin == pend ? pblank : static_cast<const void*>(&pbegin[0]));
    size_t sz = (pend - pbegin) * sizeof(pbegin[0]);
//    size_t sz = 80;

    sph_sha256_init(&h.ctx_sha256);
    // ZSHA256;
    sph_sha256 (&h.ctx_sha256, ptr, sz);
    sph_sha256_close(&h.ctx_sha256, static_cast<void*>(&hash[0]));
    
    sph_sha512_init(&h.ctx_sha512);
    // ZSHA512;
    sph_sha512 (&h.ctx_sha512, ptr, sz);
    sph_sha512_close(&h.ctx_sha512, static_cast<void*>(&hash[1]));
    
    sph_keccak512_init(&h.ctx_keccak);
    // ZKECCAK;
    sph_keccak512 (&h.ctx_keccak, ptr, sz);
    sph_keccak512_close(&h.ctx_keccak, static_cast<void*>(&hash[2]));

    sph_whirlpool_init(&h.ctx_whirlpool);
    // ZWHIRLPOOL;
    sph_whirlpool (&h.ctx_whirlpool, ptr, sz);
    sph_whirlpool_close(&h.ctx_whirlpool, static_cast<void*>(&hash[3]));
    
    sph_haval256_5_init(&h.ctx_haval);
    // ZHAVAL;
    sph_haval256_5 (&h.ctx_haval, ptr, sz);
    sph_haval256_5_close(&h.ctx_haval, static_cast<void*>(&hash[4]));

    sph_tiger_init(&h.ctx_tiger);
    // ZTIGER;
    sph_tiger (&h.ctx_tiger, ptr, sz);
    sph_tiger_close(&h.ctx_tiger, static_cast<void*>(&hash[5]));

    sph_ripemd160_init(&h.ctx_ripemd);
    // ZRIPEMD;
    sph_ripemd160 (&h.ctx_ripemd, ptr, sz);
    sph_ripemd160_close(&h.ctx_ripemd, static_cast<void*>(&hash[6]));

    //printf("%s\n", hash[6].GetHex().c_str());

    //Take care of zeros and load gmp
    for(int i=0; i < 7; i++){
	if(hash[i]==0)
	    hash[i] = 1;
	mpz_set_uint512(h.bns[i],hash[i]);
    }
 
    mpz_set_ui(h.bns[7],0);
    for(int i=0; i < 7; i++)
	mpz_add(h.bns[7], h.bns[7], h.bns[i]);

    mpz_pow_ui(h.bns[7], h.bns[7], 2);
    mpz_set_ui(h.product,1);
    for(int i=0; i < 8; i++){
	mpz_mul(h.product,h.product,h.bns[i]);
    }
    mpz_pow_ui(h.product, h.product, 2);

    bytes = mpz_sizeinbase(h.product, 256);
    char *adata = (char*)malloc(bytes);
    mpz_export(adata, NULL, -1, 1, 0, 0, h.product);

    sph_sha256_init(&h.ctx_sha256);
    // ZSHA256;
//    sph_sha256 (&h.ctx_sha256, h.data,bytes);
    sph_sha256 (&h.ctx_sha256, adata, bytes);
    sph_sha256_close(&h.ctx_sha256, static_cast<void*>(&finalhash));
    free(adata);

for(int i=0; i < NM7M; i++)
{
    if(finalhash==0) finalhash = 1;
    mpz_set_uint256(h.bns[0],finalhash);
    mpz_add(h.bns[7], h.bns[7], h.bns[0]);

    mpz_mul(h.product, h.product, h.bns[7]);
    mpz_cdiv_q (h.product, h.product, h.bns[0]);
    if (mpz_sgn(h.product) <= 0) mpz_set_ui(h.product,1);

    bytes = mpz_sizeinbase(h.product, 256);
//    printf("M7M data space: %iB\n", bytes);
    char *bdata = (char*)malloc(bytes);
    mpz_export(bdata, NULL, -1, 1, 0, 0, h.product);

    sph_sha256_init(&h.ctx_sha256);
    // ZSHA256;
    sph_sha256 (&h.ctx_sha256, bdata, bytes);
    sph_sha256_close(&h.ctx_sha256, static_cast<void*>(&finalhash));
    free(bdata);
//    printf("finalhash = %s\n", finalhash.GetHex().c_str());
}
    
    return finalhash;
}



#endif // HASHBLOCK_H

