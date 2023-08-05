#include "fat.h"
//#define  DiskInterface::GenericDiskInterface *p = osData.diskInterfaces[hd];
#define RD28(hd,S,D,C) osData.diskInterfaces[hd]->ReadBytes(S,C,D)
#define W28(hd,S,D,C) osData.diskInterfaces[hd]->WriteBytes(S,C,D)

DirectoryEntry *dirent;

static uint32_t dataStartSector;
static uint32_t sectorsPrCluster;
static uint32_t fatLocation;
static uint32_t fatSize;
static uint32_t directorySector;
static uint32_t directoryCluster;

void UpdateEntryInFat(int hd, uint32_t cluster, uint32_t newFatValue, uint32_t fatLocation){
    uint32_t  fatBuffer[512 / sizeof(uint32_t)];
    uint32_t sector = cluster / (512 / sizeof(uint32_t));                                                                                //Calculate the sector of the FAT table
    uint32_t offset = cluster % (512 / sizeof(uint32_t));                                                                                //Calculate the offset of the FAT table
    RD28(hd,fatLocation + sector, (uint8_t*)fatBuffer, sizeof(fatBuffer));              //Read the sector of the FAT table
    fatBuffer[offset] = newFatValue;                                                                                                //Set the entry to 0x0FFFFFFF
    W28(hd,fatLocation + sector, (uint8_t*)fatBuffer, sizeof(fatBuffer));             //Write the changes to the disk                                    //Write the sector of the FAT table
}

void DeallocateCluster(int hd, uint32_t firstCluster, uint32_t fatLocation, uint32_t fat_size) {
    
    //Loop through all the clusters in the file
    uint32_t nextCluster = firstCluster;
    while (nextCluster != 0x0FFFFFFF) {

        //Read the current cluster's entry in the FAT
        uint32_t  fatBuffer[512 / sizeof(uint32_t)];                                             //Create a buffer to store a sector of the FAT table
        uint32_t sector = nextCluster / (512 / sizeof(uint32_t));                                //Calculate the sector of the FAT table
        uint32_t offset = nextCluster % (512 / sizeof(uint32_t));                                //Calculate the offset of the FAT table
        RD28(hd,fatLocation + sector, (uint8_t*)fatBuffer, sizeof(fatBuffer));              //Read the sector of the FAT table

        //Mark the current cluster as free
        UpdateEntryInFat(hd, nextCluster, 0x00000000, fatLocation);

        //Get the next cluster in the file
        nextCluster = fatBuffer[offset];
    }
}

uint32_t AllocateCluster(int hd, uint32_t currentCluster, uint32_t fatLocation, uint32_t fat_size){
    
    //Find and store the first free cluster
    uint32_t  fatBuffer[512 / sizeof(uint32_t)];                                                                                        //Create a buffer to store a sector of the FAT table
    uint32_t nextFreeCluster = -1;
    for (int sector = 0; sector < fat_size; ++sector) {                                                                     //Loop the sectors of the FAT table

        RD28(hd,fatLocation + sector, (uint8_t*)fatBuffer, sizeof(fatBuffer));          //Read the sector of the FAT table
        for (int j = 0; j < 512 / sizeof(uint32_t); ++j) {                                                                              //Loop through each entry in the sectort

            if(fatBuffer[j] == 0){                                                                                                      //Fat entries are available if it is 0x000000

                nextFreeCluster = sector * (512 / sizeof(uint32_t)) + j;                                                                //Calculate the cluster number
                break;
            }
        }
    }

    //If there is no space left on the drive
    if(nextFreeCluster == -1) return nextFreeCluster;                                                                                        //If there is no free clusters then return

    //Mark the next free cluster as used
    UpdateEntryInFat(hd, nextFreeCluster, nextFreeCluster, fatLocation);

    //Mark the newly allocated cluster as the last cluster of the file
    UpdateEntryInFat(hd, nextFreeCluster, 0x0FFFFFFF, fatLocation);

    //Modify the FAT table to update the last cluster to point to the new cluster
    UpdateEntryInFat(hd, currentCluster, nextFreeCluster, fatLocation);

    return nextFreeCluster;
}

void UpdateDirectoryEntrysToDisk(int hd){
    DirectoryEntry tempDirent[16];
    
    //Directory reading  loop
    int index = 0;
    uint32_t nextCluster = directoryCluster;
    uint8_t fatBuffer[513];

    //Write directory entries until the end of the cluster
    while(true) {

        //Convert file cluster into a sector
        uint32_t directoryReadSector = dataStartSector + sectorsPrCluster*(nextCluster - 2);                  //*Offset by 2
        int sectorOffset = 0;

        //Read the secotrs in the cluster
        while(true){
            //Copy the directory entries to the tempBuffer
            for(int i = 0; i < 16; i++) {
                tempDirent[i] = *(dirent + index);
                index++;
            }

            //Write a sectors worth of directory entries
            W28(hd,directoryReadSector + sectorOffset, (uint8_t*)&tempDirent[0], 16*sizeof(DirectoryEntry));      //Read the directory entries
            sectorOffset++;                                                                                             //Increment the sector offset


            //If the next sector is in a different cluster then break
            if (sectorOffset > sectorsPrCluster)
                break;
        }

        //Get the next cluster of the directory
        uint32_t sector = nextCluster / (512 / sizeof(uint32_t));                                                                       //Calculate the sector of the FAT table
        uint32_t offset = nextCluster % (512 / sizeof(uint32_t));                                                                       //Calculate the offset of the FAT table

        //Read the FAT sector for the current cluster
        RD28(hd,fatLocation + sector, fatBuffer, 512);

        //Set the next cluster to the next cluster in the FAT
        nextCluster = ((uint32_t*)&fatBuffer)[offset] & 0x0FFFFFFF;

        //if the next cluster is 0, it means the end of the cluster chain
        if (nextCluster == 0) {
            break;
        }
    }

    //TODO: Expand the directory if there is not enough space

}

void MakeDirectory(int hd,char *name) {

    //Check if the name is a valid FAT32 name
    //if(!Fat32::IsValidFAT32Name(name)) return;

    //Check if the name is already in use
    for(int i = 0; i < 16; i++) {

        uint8_t ss = ((DirectoryEntry)(*(dirent + i))).attributes;
        uint8_t tname = ((DirectoryEntry)(*(dirent + i))).name[0];
        if ((ss & 0x0F) == 0x0F || (ss & 0x10) != 0x10)  //If the atrribute is 0x0F then this is a long file name entry, skip it. Or if its not a directory
            continue;

        if (tname == 0x00)                                  //If the name is 0x00 then there are no more entries
            break;

        if (tname == 0xE5)                                  //If the name is 0xE5 then the entry is free
            continue;

    }

    //Create a new directory entry with the name
    DirectoryEntry newDir;                                      //Directory entry to be added
    newDir.attributes = 0x10;                                   //Set the attributes to directory
    newDir.size = 0;                                            //Set the size to 0
    for(int i = 0; i < 8; i++) {                                //Set the name
        newDir.name[i] = name[i];
    }

	//Allocate a cluster for the directory
    uint32_t cluster = AllocateCluster(hd, fatLocation, fatSize, sectorsPrCluster);

    //Convert the cluster into high and low for the directory entry
    newDir.firstClusterHigh = (uint16_t)(cluster >> 16);
    newDir.firstClusterLow = (uint16_t)(cluster & 0xFFFF);

    //Find the first free directory entry
    for(int i = 0; i < 16; i++) {
        uint8_t tname = ((DirectoryEntry)(*(dirent + i*sizeof(DirectoryEntry)))).name[0];
        //uint8_t tname2 = *(dirent + i)->name[0];
        if (tname == 0x00) {                            //If the name is 0x00 then there are no more entries
            *(dirent + i) = (DirectoryEntry)newDir;                                     //Set the directory entry to the new directory entry
            break;
        }
    }

    //Write the directory entry to the disk
    UpdateDirectoryEntrysToDisk(hd);

    //Create the "." and ".." directory entries
    DirectoryEntry thisDir;                                         //Directory entry for the "." directory, this points to the current directory
    DirectoryEntry parentDir;                                       //Directory entry for the ".." directory, this points to the parent directory

}