#include "uint256.h"

#pragma pack(push,1)
class CBlockHeader
{
public:
    //!!!!!!!!!!! struct must be in packed order even though serialize order is version first
    //or else we can't use hash macros, could also use #pragma pack but that has 
    //terrible implicatation on non-x86
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
	    unsigned char Padding[48];
};
#pragma pack(pop)
