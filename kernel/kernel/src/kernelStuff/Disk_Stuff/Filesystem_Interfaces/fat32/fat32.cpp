#include "fat32.h"

size_t offset(const BiosParameterBlock* bpb, uint32_t cluster) {
    return (bpb->reversed_sector
            + (bpb->fats * bpb->sectors_per_fat_32)
            + ((cluster - 2) * bpb->sectors_per_cluster))
            * bpb->bytes_per_sector;
}

uint32_t RetFirstSectorCluser(BiosParameterBlock *temp,
        uint32_t DataStartSector,uint32_t N_cluster){
    return DataStartSector + (N_cluster - 2) * temp->sectors_per_fat_32;
}

unsigned char ChkSum (unsigned char *pFcbName)
{
    short FcbNameLen;
    unsigned char Sum;

    Sum = 0;
    for (FcbNameLen=11; FcbNameLen!=0; FcbNameLen--) {
        // NOTE: The operation is an unsigned char rotate right
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + *pFcbName++;
    }
    return (Sum);
}