/* CMSIS-DAP Interface Firmware
 * Copyright (c) 2009-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RTL.h"
#include "rl_usb.h"
#include "string.h"

#include "main.h"
#include "target_reset.h"
#include "target_flash.h"
#include "usb_buf.h"
#include "virtual_fs.h"
#include "daplink_debug.h"

#include "version.h"

void usbd_msc_init ()
{    
    // config the mbr and FAT - FAT may be const later
    virtual_fs_init();
    
    USBD_MSC_MemorySize = mbr.bytes_per_sector * mbr.total_logical_sectors;
    USBD_MSC_BlockSize  = mbr.bytes_per_sector;
    USBD_MSC_BlockGroup = 1;
    USBD_MSC_BlockCount = USBD_MSC_MemorySize / USBD_MSC_BlockSize;
    USBD_MSC_BlockBuf   = (uint8_t *)usb_buffer;

    USBD_MSC_MediaReady = __TRUE;
}

void usbd_msc_read_sect (U32 block, U8 *buf, U32 num_of_blocks)
{
    fs_entry_t fs_read = {0,0};
    uint32_t max_known_fs_entry_addr = 0;
    uint32_t req_sector_offset = 0;
    uint32_t req_addr = block * USBD_MSC_BlockSize;
    uint8_t i = 0, real_data_present = 1;
    
    // dont proceed if we're not ready
    if (!USBD_MSC_MediaReady) {
        return;
    }
    
    // indicate msc activity
    main_blink_msd_led(0);
    
    // A block is requested from the host. We dont have a flat file system image on disc
    //  rather just the required bits (mbr, fat, root dir, file data). The fs structure 
    //  how these parts look without requiring them all to exist
    while((fs[i].length != 0) && (fs_read.sect == 0)) {
        // accumulate the length of the fs.sect(s) we have examined so far
        max_known_fs_entry_addr += fs[i].length;
        // determine if we have real system data or need to pad the transfer with 0
        if (req_addr < max_known_fs_entry_addr) {
            // we know this is where the data request is, store it for later transmission
            fs_read.sect = fs[i].sect;
            // sector can be larger than a block. Normalize the block number into the fs entry
            req_sector_offset = fs[i].length - (max_known_fs_entry_addr - req_addr);
            // determine if the inflated size is greater than the real size.
            if(req_sector_offset >= sizeof(fs[i].sect)) {
                real_data_present = 0;
            }
        }
        i++;
    }
    // now send the data if a known sector and valid data in memory - otherwise send 0's
    if (fs_read.sect != 0 && real_data_present == 1) {
        memcpy(buf, &fs_read.sect[req_sector_offset], num_of_blocks * USBD_MSC_BlockSize);
    }
    else {
        memset(buf, 0, num_of_blocks * USBD_MSC_BlockSize);
    }
    // tmp way to send the known dynamic files. Need to calculate the location
    if (block == 17) {
        update_html_file();
    }
}

// known extension types
// CRD - chrome
// PAR - IE
// Extensions dont matter much if you're looking for specific file data
//  other than size parsing but hex and srec have specific EOF records
static const char *known_extensions[] = {
    "BIN",
    "bin",
    0,
};

typedef enum extension {
    UNKNOWN = 0,
    BIN,
} extension_t;

static uint32_t first_byte_valid(uint8_t c)
{
    const char *valid_char = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    do {
        if(c == *valid_char) {
            return 1;
        }
    } while (*(valid_char++) != '\0');
    return 0;
}        

static extension_t wanted_dir_entry(const FatDirectoryEntry_t dir_entry)
{
    uint32_t i = 0;
    // we may see the following. Validate all or keep looking
    //  entry with invalid or reserved first byte
    //  entry with a false filesize.
    while (known_extensions[i] != 0) {
        if(1 == first_byte_valid(dir_entry.filename[0])) {
            if (0 == strncmp(known_extensions[i], (const char *)&dir_entry.filename[8], 3)) {
                return (dir_entry.filesize) ? BIN : UNKNOWN;
            }
        }
        i++;
    }
    return UNKNOWN;
}

void usbd_msc_write_sect (U32 block, U8 *buf, U32 num_of_blocks)
{
    FatDirectoryEntry_t tmp_file = {0};
    uint32_t i = 0;
    static struct {
        uint32_t start_block;
        uint32_t amt_to_write;
        uint32_t amt_written;
        uint32_t last_block_written;
        uint32_t transfer_started;
        extension_t file_type;
    } file_transfer_state = {0,0,0,0,0,UNKNOWN};
    
    if (!USBD_MSC_MediaReady) {
        return;
    }
    
    debug_msg("block: %d\r\n", block);
    // indicate msd activity
    main_blink_msd_led(0);
      
    // this is the key for starting a file write - we dont care what file types are sent
    //  just look for something unique (NVIC table, hex, srec, etc)
    if (1 == validate_bin_nvic(buf) && file_transfer_state.transfer_started == 0) {
        debug_msg("%s", "FLASH INIT\r\n");
        
        // binary file transfer - reset parsing
        file_transfer_state.start_block = block;
        file_transfer_state.amt_to_write = 0xffffffff;
        file_transfer_state.amt_written = USBD_MSC_BlockSize;
        file_transfer_state.last_block_written = block;
        file_transfer_state.transfer_started = 1;
        file_transfer_state.file_type = BIN;
        
        // prepare the target device
        if (0 == target_flash_init()) {
            // we failed here INIT
            main_usb_disconnect_event();
        }
        // writing in 2 places less than ideal but manageable
        debug_msg("%s", "FLASH WRITE\r\n");
        //debug_data(buf, USBD_MSC_BlockSize);
        if (0 == target_flash_program_page((block-file_transfer_state.start_block)*USBD_MSC_BlockSize, buf, USBD_MSC_BlockSize)) {
            // we failed here ERASE, WRITE
            main_usb_disconnect_event();
        }
        return;
    }
    // if the root dir comes we should look at it and parse for info that can end a transfer
    else if ((block == ((mbr.num_fats * mbr.logical_sectors_per_fat) + 1)) || 
             (block == ((mbr.num_fats * mbr.logical_sectors_per_fat) + 2))) {
        // start looking for a known file and some info about it
        for( ; i < USBD_MSC_BlockSize/sizeof(tmp_file); i++) {
            memcpy(&tmp_file, &buf[i*sizeof(tmp_file)], sizeof(tmp_file));
            debug_msg("na:%.11s\tatrb:%8d\tsz:%8d\tst:%8d\tcr:%8d\tmod:%8d\taccd:%8d\r\n"
                , tmp_file.filename, tmp_file.attributes, tmp_file.filesize, tmp_file.first_cluster_low_16
                , tmp_file.creation_time_ms, tmp_file.modification_time, tmp_file.accessed_date);
            // ToDO: get the extension from the parser for verification
            if (wanted_dir_entry(tmp_file)) {
                // sometimes a 0 files size is written while a transfer is in progress. This isnt cool
                file_transfer_state.amt_to_write = (tmp_file.filesize > 0) ? tmp_file.filesize : 0xffffffff;
            }
        }
    }

    // write data to media
    if ((block >= file_transfer_state.start_block) && 
        (file_transfer_state.transfer_started == 1)) {
        if (block >= file_transfer_state.start_block) {
            // check for contiguous transfer
            if (block != (file_transfer_state.last_block_written+1)) {
                // this is non-contigous transfer. need to wait for then next proper block
                debug_msg("%s", "BLOCK OUT OF ORDER\r\n");
            }
            else {
                debug_msg("%s", "FLASH WRITE\r\n");
                //debug_data(buf, USBD_MSC_BlockSize);
                if (0 == target_flash_program_page((block-file_transfer_state.start_block)*USBD_MSC_BlockSize, buf, USBD_MSC_BlockSize)) {
                    // we failed here ERASE, WRITE
                    main_usb_disconnect_event();
                }
                // and do the housekeeping
                file_transfer_state.amt_written += USBD_MSC_BlockSize;
                file_transfer_state.last_block_written = block;
            }
        }
    }
    
    // see if a complete transfer occured by knowing it started and comparing filesize expectations
    if ((file_transfer_state.amt_written >= file_transfer_state.amt_to_write) && (file_transfer_state.transfer_started == 1 )){
        // do the disconnect - maybe write some programming stats to the file
        debug_msg("%s", "FLASH END\r\n");
        // we know the contents have been reveived. Time to eject
        file_transfer_state.transfer_started = 0;
        main_usb_disconnect_event();
    }
    
    // There is one more known state where the root dir is updated with the amount of data transfered but not the whole file transfer was complete
    //  To handle this we need a state to kick off a timer for a fixed amount of time where we can receive more continous secotrs and assume
    //  they are valid file data. This is only the case for bin files since the only known end is the filesize from the root dir entry.
}





























//static const FatDirectoryEntry_t drive = {
//    /*uint8_t[11] */ .filename = "MBED       ",
//    /*uint8_t */ .attributes = 0x28,
//    /*uint8_t */ .reserved = 0x00,
//    /*uint8_t */ .creation_time_ms = 0x00,
//    /*uint16_t*/ .creation_time = 0x0000,
//    /*uint16_t*/ .creation_date = 0x0000,
//    /*uint16_t*/ .accessed_date = 0x0000,
//    /*uint16_t*/ .first_cluster_high_16 = 0x0000,
//    /*uint16_t*/ .modification_time = 0x8E41,
//    /*uint16_t*/ .modification_date = 0x32bb,
//    /*uint16_t*/ .first_cluster_low_16 = 0x0000,
//    /*uint32_t*/ .filesize = 0x00000000,
//};


//static const FatDirectoryEntry_t file1 = {
//    /*uint8_t[11] */ .filename = "ABOUT   TXT",
//    /*uint8_t */ .attributes = 0x21,
//    /*uint8_t */ .reserved = 0x00,
//    /*uint8_t */ .creation_time_ms = 0x00,
//    /*uint16_t*/ .creation_time = 0x0000,
//    /*uint16_t*/ .creation_date = 0x0021,
//    /*uint16_t*/ .accessed_date = 0xbb32,
//    /*uint16_t*/ .first_cluster_high_16 = 0x0000,
//    /*uint16_t*/ .modification_time = 0x83dc,
//    /*uint16_t*/ .modification_date = 0x32bb,
//    /*uint16_t*/ .first_cluster_low_16 = 0x0002,
//    /*uint32_t*/ .filesize = sizeof(file1_contents)
//};

//static const uint8_t fat2[] = {0};

//static const uint8_t fail[] = {
//    'F','A','I','L',' ',' ',' ',' ',                   // Filename
//    'T','X','T',                                       // Filename extension
//    0x20,                                              // File attributes
//    0x18,0xB1,0x74,0x76,0x8E,0x41,0x8E,0x41,0x00,0x00, // Reserved
//    0x8E,0x76,                                         // Time created or last updated
//    0x8E,0x41,                                         // Date created or last updated
//    0x06,0x00,                                         // Starting cluster number for file
//    0x07,0x00,0x00,0x0                                 // File size in bytes
//};

//// first 16 of the max 32 (mbr.max_root_dir_entries) root dir entries
//static const uint8_t root_dir1[] = {
//    // volume label "MBED"
//    'M', 'B', 'E', 'D', 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x28, 0x0, 0x0, 0x0, 0x0,
//    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x85, 0x75, 0x8E, 0x41, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // Hidden files to keep mac happy

//    // .fseventsd (LFN + normal entry)  (folder, size 0, cluster 2)
//    0x41, 0x2E, 0x0, 0x66, 0x0, 0x73, 0x0, 0x65, 0x0, 0x76, 0x0, 0xF, 0x0, 0xDA, 0x65, 0x0,
//    0x6E, 0x0, 0x74, 0x0, 0x73, 0x0, 0x64, 0x0, 0x0, 0x0, 0x0, 0x0, 0xFF, 0xFF, 0xFF, 0xFF,

//    0x46, 0x53, 0x45, 0x56, 0x45, 0x4E, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x12, 0x0, 0x47, 0x7D, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x7D, 0x75, 0x8E, 0x41, 0x2, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // .metadata_never_index (LFN + LFN + normal entry)  (size 0, cluster 0)
//    0x42, 0x65, 0x0, 0x72, 0x0, 0x5F, 0x0, 0x69, 0x0, 0x6E, 0x0, 0xF, 0x0, 0xA8, 0x64, 0x0,
//    0x65, 0x0, 0x78, 0x0, 0x0, 0x0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0, 0x0, 0xFF, 0xFF, 0xFF, 0xFF,

//    0x1, 0x2E, 0x0, 0x6D, 0x0, 0x65, 0x0, 0x74, 0x0, 0x61, 0x0, 0xF, 0x0, 0xA8, 0x64, 0x0,
//    0x61, 0x0, 0x74, 0x0, 0x61, 0x0, 0x5F, 0x0, 0x6E, 0x0, 0x0, 0x0, 0x65, 0x0, 0x76, 0x0,

//    0x4D, 0x45, 0x54, 0x41, 0x44, 0x41, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x22, 0x0, 0x32, 0x85, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x85, 0x75, 0x8E, 0x41, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // .Trashes (LFN + normal entry)  (size 0, cluster 0)
//    0x41, 0x2E, 0x0, 0x54, 0x0, 0x72, 0x0, 0x61, 0x0, 0x73, 0x0, 0xF, 0x0, 0x25, 0x68, 0x0,
//    0x65, 0x0, 0x73, 0x0, 0x0, 0x0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0, 0x0, 0xFF, 0xFF, 0xFF, 0xFF,

//    0x54, 0x52, 0x41, 0x53, 0x48, 0x45, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x22, 0x0, 0x32, 0x85, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x85, 0x75, 0x8E, 0x41, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // Hidden files to keep windows 8.1 happy
//    0x42, 0x20, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x66, 0x00, 0x6F, 0x00, 0x0F, 0x00, 0x72, 0x72, 0x00,
//    0x6D, 0x00, 0x61, 0x00, 0x74, 0x00, 0x69, 0x00, 0x6F, 0x00, 0x00, 0x00, 0x6E, 0x00, 0x00, 0x00,

//    0x01, 0x53, 0x00, 0x79, 0x00, 0x73, 0x00, 0x74, 0x00, 0x65, 0x00, 0x0F, 0x00, 0x72, 0x6D, 0x00,
//    0x20, 0x00, 0x56, 0x00, 0x6F, 0x00, 0x6C, 0x00, 0x75, 0x00, 0x00, 0x00, 0x6D, 0x00, 0x65, 0x00,

//    0x53, 0x59, 0x53, 0x54, 0x45, 0x4D, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x16, 0x00, 0xA5, 0x85, 0x8A,
//    0x73, 0x43, 0x73, 0x43, 0x00, 0x00, 0x86, 0x8A, 0x73, 0x43, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,

//    // mbed html file (size 512, cluster 3)
//    'M', 'B', 'E', 'D', 0x20, 0x20, 0x20, 0x20, 'H', 'T', 'M', 0x20, 0x18, 0xB1, 0x74, 0x76,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x8E, 0x76, 0x8E, 0x41, 0x05, 0x0, 0x00, 0x02, 0x0, 0x0,
//};

// last 16 of the max 32 (mbr.max_root_dir_entries) root dir entries
//static const uint8_t root_dir2[] = {0};

//static const uint8_t sect5[] = {
//    // .   (folder, size 0, cluster 2)
//    0x2E, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x32, 0x0, 0x47, 0x7D, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x88, 0x75, 0x8E, 0x41, 0x2, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // ..   (folder, size 0, cluster 0)
//    0x2E, 0x2E, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x0, 0x47, 0x7D, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x7D, 0x75, 0x8E, 0x41, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,

//    // NO_LOG  (size 0, cluster 0)
//    0x4E, 0x4F, 0x5F, 0x4C, 0x4F, 0x47, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x8, 0x32, 0x85, 0x75,
//    0x8E, 0x41, 0x8E, 0x41, 0x0, 0x0, 0x85, 0x75, 0x8E, 0x41, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
//};

//static const uint8_t sect6[] = {
//    0x2E, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x00, 0xA5, 0x85, 0x8A,
//    0x73, 0x43, 0x73, 0x43, 0x00, 0x00, 0x86, 0x8A, 0x73, 0x43, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,

//    0x2E, 0x2E, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x00, 0xA5, 0x85, 0x8A,
//    0x73, 0x43, 0x73, 0x43, 0x00, 0x00, 0x86, 0x8A, 0x73, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

//    // IndexerVolumeGuid (size0, cluster 0)
//    0x42, 0x47, 0x00, 0x75, 0x00, 0x69, 0x00, 0x64, 0x00, 0x00, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF,
//    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,

//    0x01, 0x49, 0x00, 0x6E, 0x00, 0x64, 0x00, 0x65, 0x00, 0x78, 0x00, 0x0F, 0x00, 0xFF, 0x65, 0x00,
//    0x72, 0x00, 0x56, 0x00, 0x6F, 0x00, 0x6C, 0x00, 0x75, 0x00, 0x00, 0x00, 0x6D, 0x00, 0x65, 0x00,

//    0x49, 0x4E, 0x44, 0x45, 0x58, 0x45, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x20, 0x00, 0xA7, 0x85, 0x8A,
//    0x73, 0x43, 0x73, 0x43, 0x00, 0x00, 0x86, 0x8A, 0x73, 0x43, 0x04, 0x00, 0x4C, 0x00, 0x00, 0x00
//};

//static const uint8_t sect7[] = {
//	0x7B, 0x00, 0x39, 0x00, 0x36, 0x00, 0x36, 0x00, 0x31, 0x00, 0x39, 0x00, 0x38, 0x00, 0x32, 0x00,
//	0x30, 0x00, 0x2D, 0x00, 0x37, 0x00, 0x37, 0x00, 0x44, 0x00, 0x31, 0x00, 0x2D, 0x00, 0x34, 0x00,
//	0x46, 0x00, 0x38, 0x00, 0x38, 0x00, 0x2D, 0x00, 0x38, 0x00, 0x46, 0x00, 0x35, 0x00, 0x33, 0x00,
//	0x2D, 0x00, 0x36, 0x00, 0x32, 0x00, 0x44, 0x00, 0x39, 0x00, 0x37, 0x00, 0x46, 0x00, 0x35, 0x00,
//	0x46, 0x00, 0x34, 0x00, 0x46, 0x00, 0x46, 0x00, 0x39, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00
//};


//SECTOR sectors[] = {
//    /* Reserved Sectors: Master Boot Record */
//    {(const uint8_t *)&mbr , sizeof(mbr)},

//    /* FAT Region: FAT1 */
//    {(const uint8_t *)&fat1, sizeof(fat1)},   // fat1, sect1
//    
//    EMPTY_FAT_SECTORS

//    /* FAT Region: FAT2 */
//    {fat2, 0},              // fat2, sect1
//    EMPTY_FAT_SECTORS

//    /* Root Directory Region */
//    //{root_dir1, sizeof(root_dir1)}, // first 16 of the max 32 (mbr.max_root_dir_entries) root dir entries
//    //{root_dir2, sizeof(root_dir2)}, // last 16 of the max 32 (mbr.max_root_dir_entries) root dir entries

//    /* Data Region */

//    // Section for mac compatibility
//    //{sect5, sizeof(sect5)},
//    //{sect6, sizeof(sect6)},
//    //{sect7, sizeof(sect7)},

//    // contains mbed.htm
//    //{(const uint8_t *)usb_buffer, 512},
//};

//typedef struct file_system {
//    mbr_t *br;
//    
//    fat_t *fat;
//    
//    FatDirectoryEntry_t *root;
//    FatDirectoryEntry_t *f1;
//        
//} file_system_t;

//#if defined(DBG_LPC1768)
//#   define WANTED_SIZE_IN_KB                        (512)
//#elif defined(DBG_KL02Z)
//#   define WANTED_SIZE_IN_KB                        (32)
//#elif defined(DBG_KL05Z)
//#   define WANTED_SIZE_IN_KB                        (32)
//#elif defined(DBG_K24F256)
//#   define WANTED_SIZE_IN_KB                        (256)
//#elif defined(DBG_KL25Z)
//#   define WANTED_SIZE_IN_KB                        (128)
//#elif defined(DBG_KL26Z)
//#   define WANTED_SIZE_IN_KB                        (128)
//#elif defined(DBG_KL46Z)
//#   define WANTED_SIZE_IN_KB                        (256)
//#elif defined(DBG_K20D50M)
//#   define WANTED_SIZE_IN_KB                        (128)
//#elif defined(DBG_K22F)
//#   define WANTED_SIZE_IN_KB                        (512)
//#elif defined(DBG_K64F)
//#   define WANTED_SIZE_IN_KB                        (1024)
//#elif defined(DBG_LPC812)
//#   define WANTED_SIZE_IN_KB                        (16)
//#elif defined(DBG_LPC1114)
//#   define WANTED_SIZE_IN_KB                        (32)
//#elif defined(DBG_LPC4330)
//#   if defined(BOARD_BAMBINO_210E)
//#       define WANTED_SIZE_IN_KB                    (8192)
//#   else
//#       define WANTED_SIZE_IN_KB                    (4096)
//#   endif
//#elif defined(DBG_LPC1549)
//#   define WANTED_SIZE_IN_KB                        (512)
//#elif defined(DBG_LPC11U68)
//#   define WANTED_SIZE_IN_KB                        (256)
//#elif defined(DBG_LPC4337)
//#   define WANTED_SIZE_IN_KB                        (1024)
//#endif

////------------------------------------------------------------------- CONSTANTS
//#define WANTED_SIZE_IN_BYTES        ((WANTED_SIZE_IN_KB + 16 + 8)*1024)
//#define WANTED_SECTORS_PER_CLUSTER  (8)

//#define FLASH_PROGRAM_PAGE_SIZE         (512)
//#define MBR_BYTES_PER_SECTOR            (512)

////--------------------------------------------------------------------- DERIVED

//#define MBR_NUM_NEEDED_SECTORS  (WANTED_SIZE_IN_BYTES / MBR_BYTES_PER_SECTOR)
//#define MBR_NUM_NEEDED_CLUSTERS (MBR_NUM_NEEDED_SECTORS / WANTED_SECTORS_PER_CLUSTER)

///* Need 3 sectors/FAT for every 1024 clusters */
//#define MBR_SECTORS_PER_FAT     (3*((MBR_NUM_NEEDED_CLUSTERS + 1023)/1024))

///* Macro to help fill the two FAT tables with the empty sectors without
//   adding a lot of test #ifs inside the sectors[] declaration below */
//#if (MBR_SECTORS_PER_FAT == 1)
//#   define EMPTY_FAT_SECTORS
//#elif (MBR_SECTORS_PER_FAT == 2)
//#   define EMPTY_FAT_SECTORS  {fat2,0},
//#elif (MBR_SECTORS_PER_FAT == 3)
//#   define EMPTY_FAT_SECTORS  {fat2,0},{fat2,0},
//#elif (MBR_SECTORS_PER_FAT == 6)
//#   define EMPTY_FAT_SECTORS  {fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},
//#elif (MBR_SECTORS_PER_FAT == 9)
//#   define EMPTY_FAT_SECTORS  {fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},
//#elif (MBR_SECTORS_PER_FAT == 12)
//#   define EMPTY_FAT_SECTORS  {fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},{fat2,0},
//#else
//#   error "Unsupported number of sectors per FAT table"
//#endif

//#define DIRENTS_PER_SECTOR  (MBR_BYTES_PER_SECTOR / sizeof(FatDirectoryEntry_t))

//#define SECTORS_ROOT_IDX        (1 + mbr.num_fats*MBR_SECTORS_PER_FAT)
//#define SECTORS_FIRST_FILE_IDX  (SECTORS_ROOT_IDX + 2)
//#define SECTORS_SYSTEM_VOLUME_INFORMATION (SECTORS_FIRST_FILE_IDX  + WANTED_SECTORS_PER_CLUSTER)
//#define SECTORS_INDEXER_VOLUME_GUID       (SECTORS_SYSTEM_VOLUME_INFORMATION + WANTED_SECTORS_PER_CLUSTER)
//#define SECTORS_MBED_HTML_IDX   (SECTORS_INDEXER_VOLUME_GUID + WANTED_SECTORS_PER_CLUSTER)
//#define SECTORS_ERROR_FILE_IDX  (SECTORS_MBED_HTML_IDX + WANTED_SECTORS_PER_CLUSTER)

////---------------------------------------------------------------- VERIFICATION

///* Sanity check */
//#if (MBR_NUM_NEEDED_CLUSTERS > 4084)
//  /* Limited by 12 bit cluster addresses, i.e. 2^12 but only 0x002..0xff5 can be used */
//#   error Too many needed clusters, increase WANTED_SECTORS_PER_CLUSTER
//#endif

//#if ((WANTED_SECTORS_PER_CLUSTER * MBR_BYTES_PER_SECTOR) > 32768)
//#   error Cluster size too large, must be <= 32KB
//#endif

////-------------------------------------------------------------------- TYPEDEFS


//typedef enum {
//    BIN_FILE,
//    PAR_FILE,
//    DOW_FILE,
//    CRD_FILE,
//    SPI_FILE,
//    UNSUP_FILE, /* Valid extension, but not supported */
//    SKIP_FILE,  /* Unknown extension, typically Long File Name entries */
//} FILE_TYPE;

//typedef struct {
//    FILE_TYPE type;
//    char extension[3];
//    uint32_t flash_offset;
//} FILE_TYPE_MAPPING;

    
//typedef struct file {
//    FatDirectoryEntry_t files[];
//}file_t;

//file_t ff = {
//    {
//        drive, 
//        file1
//    }
//};

//static uint32_t size;
//static uint32_t nb_sector;
//static uint32_t current_sector;
//static uint8_t sector_received_first;
//static uint8_t root_dir_received_first;
//static uint32_t jtag_flash_init;
//static uint32_t flashPtr;
//static uint8_t need_restart_usb;
//static uint8_t flash_started;
//static uint32_t start_sector;
//static uint32_t theoretical_start_sector = 7;
//static uint8_t msc_event_timeout;
//static uint8_t good_file;
//static uint8_t program_page_error;
//static uint8_t maybe_erase;
//static uint32_t previous_sector;
//static uint32_t begin_sector;
//static uint8_t task_first_started;
//static uint8_t listen_msc_isr = 1;
//static uint8_t drag_success = 1;
//static uint8_t reason = 0;
//static uint32_t flash_addr_offset = 0;

//#define SWD_ERROR               0
//#define BAD_EXTENSION_FILE      1
//#define NOT_CONSECUTIVE_SECTORS 2
//#define SWD_PORT_IN_USE         3
//#define RESERVED_BITS           4
//#define BAD_START_SECTOR        5
//#define TIMEOUT                 6

//static uint8_t * reason_array[] = {
//    "SWD ERROR",
//    "BAD EXTENSION FILE",
//    "NOT CONSECUTIVE SECTORS",
//    "SWD PORT IN USE",
//    "RESERVED BITS",
//    "BAD START SECTOR",
//    "TIMEOUT",
//};

//#define MSC_TIMEOUT_SPLIT_FILES_EVENT   (0x1000)
//#define MSC_TIMEOUT_START_EVENT         (0x2000)
//#define MSC_TIMEOUT_STOP_EVENT          (0x4000)
//#define MSC_TIMEOUT_RESTART_EVENT       (0x8000)

//// 30 s timeout
//#define TIMEOUT_S 3000

//static U64 msc_task_stack[MSC_TASK_STACK/8];

// Reference to the msc task
//static OS_TID msc_valid_file_timeout_task_id;

//static void init(uint8_t jtag);
//static void initDisconnect(uint8_t success);

// this task is responsible to check
// when we receive a root directory where there
// is a valid .bin file and when we have received
// all the sectors that we don't receive new valid sectors
// after a certain timeout
//__task void msc_valid_file_timeout_task(void) {
//    uint32_t flags = 0;
//    OS_RESULT res;
//    uint32_t start_timeout_time = 0, time_now = 0;
//    uint8_t timer_started = 0;
//    msc_valid_file_timeout_task_id = os_tsk_self();
//    while (1) {
//        res = os_evt_wait_or(MSC_TIMEOUT_SPLIT_FILES_EVENT | MSC_TIMEOUT_START_EVENT | MSC_TIMEOUT_STOP_EVENT | MSC_TIMEOUT_RESTART_EVENT, 100);

//        if (res == OS_R_EVT) {

//            flags = os_evt_get();

//            if (flags & MSC_TIMEOUT_SPLIT_FILES_EVENT) {
//                msc_event_timeout = 1;
//                os_dly_wait(50);

//                if (msc_event_timeout == 1) {
//                    // if the program reaches this point -> it means that no sectors have been received in the meantime
//                    initDisconnect(1);
//                    msc_event_timeout = 0;
//                }
//            }

//            if (flags & MSC_TIMEOUT_START_EVENT) {
//                start_timeout_time = os_time_get();
//                timer_started = 1;
//            }

//            if (flags & MSC_TIMEOUT_STOP_EVENT) {
//                timer_started = 0;
//            }

//            if (flags & MSC_TIMEOUT_RESTART_EVENT) {
//                if (timer_started) {
//                    start_timeout_time = os_time_get();
//                }
//            }

//        } else {
//            if (timer_started) {
//                time_now = os_time_get();
//                // timeout
//                if ((time_now - start_timeout_time) > TIMEOUT_S) {
//                    timer_started = 0;
//                    reason = TIMEOUT;
//                    initDisconnect(0);
//                }
//            }
//        }
//    }
//}

//void init(uint8_t jtag) {
//    size = 0;
//    nb_sector = 0;
//    current_sector = 0;
//    if (jtag) {
//        jtag_flash_init = 0;
//        theoretical_start_sector = (drag_success) ? 7 : 8;
//        good_file = 0;
//        program_page_error = 0;
//        maybe_erase = 0;
//        previous_sector = 0;
//    }
//    begin_sector = 0;
//    flashPtr = 0;
//    sector_received_first = 0;
//    root_dir_received_first = 0;
//    need_restart_usb = 0;
//    flash_started = 0;
//    start_sector = 0;
//    msc_event_timeout = 0;
//    USBD_MSC_BlockBuf   = (uint8_t *)usb_buffer;
//    listen_msc_isr = 1;
//    flash_addr_offset = 0;
//}

//void failSWD() {
//    reason = SWD_ERROR;
//    initDisconnect(0);
//}

//extern DAP_Data_t DAP_Data;  // DAP_Data.debug_port

//#ifdef BOARD_UBLOX_C027
//#include "read_uid.h"
//#endif

//static void initDisconnect(uint8_t success) {
//#if defined(BOARD_UBLOX_C027)
//    int autorst = (good_file == 2) && success;
//    int autocrp = (good_file == 3) && success;
//    if (autocrp)
//    {
//        // first we need to discoonect the usb stack
//        usbd_connect(0);

//        enter_isp();
//    }
//#else
//    int autorst = 0;
//#endif
//    drag_success = success;
//    if (autorst)
//        swd_set_target_state(RESET_RUN);
//    main_blink_msd_led(0);
//    init(1);
//    isr_evt_set(MSC_TIMEOUT_STOP_EVENT, msc_valid_file_timeout_task_id);
//    if (!autorst)
//    {
//        // event to disconnect the usb
//        main_usb_disconnect_event();
//    }
//    semihost_enable();
//}

//extern uint32_t SystemCoreClock;

//int jtag_init() {
//    if (DAP_Data.debug_port != DAP_PORT_DISABLED) {
//        need_restart_usb = 1;
//    }

//    if ((jtag_flash_init != 1) && (DAP_Data.debug_port == DAP_PORT_DISABLED)) {
//        if (need_restart_usb == 1) {
//            reason = SWD_PORT_IN_USE;
//            initDisconnect(0);
//            return 1;
//        }

//        semihost_disable();

//        PORT_SWD_SETUP();

//        target_set_state(RESET_PROGRAM);
//        if (!target_flash_init(SystemCoreClock)) {
//            failSWD();
//            return 1;
//        }

//        jtag_flash_init = 1;
//    }
//    return 0;
//}


//static const FILE_TYPE_MAPPING file_type_infos[] = {
//    { BIN_FILE, {'B', 'I', 'N'}, 0x00000000 },
//    { BIN_FILE, {'b', 'i', 'n'}, 0x00000000 },
//    { PAR_FILE, {'P', 'A', 'R'}, 0x00000000 },//strange extension on win IE 9...
//    { DOW_FILE, {'D', 'O', 'W'}, 0x00000000 },//strange extension on mac...
//    { CRD_FILE, {'C', 'R', 'D'}, 0x00000000 },//strange extension on linux...
//    { UNSUP_FILE, {0,0,0},     0            },//end of table marker
//};

//static FILE_TYPE get_file_type(const FatDirectoryEntry_t* pDirEnt, uint32_t* pAddrOffset) {
//    int i;
//    char e0 = pDirEnt->filename[8];
//    char e1 = pDirEnt->filename[9];
//    char e2 = pDirEnt->filename[10];
//    char f0 = pDirEnt->filename[0];
//    for (i = 0; file_type_infos[i].type != UNSUP_FILE; i++) {
//        if ((e0 == file_type_infos[i].extension[0]) &&
//            (e1 == file_type_infos[i].extension[1]) &&
//            (e2 == file_type_infos[i].extension[2])) {
//            *pAddrOffset = file_type_infos[i].flash_offset;
//            return file_type_infos[i].type;
//        }
//    }

//    // Now test if the file has a valid extension and a valid name.
//    // This is to detect correct but unsupported 8.3 file names.
//    if (( ((e0 >= 'a') && (e0 <= 'z')) || ((e0 >= 'A') && (e0 <= 'Z')) ) &&
//        ( ((e1 >= 'a') && (e1 <= 'z')) || ((e1 >= 'A') && (e1 <= 'Z')) || (e1 == 0x20) ) &&
//        ( ((e2 >= 'a') && (e2 <= 'z')) || ((e2 >= 'A') && (e2 <= 'Z')) || (e2 == 0x20) ) &&
//        ( ((f0 >= 'a') && (f0 <= 'z')) || ((f0 >= 'A') && (f0 <= 'Z')) ) &&
//           (f0 != '.' &&
//           (f0 != '_')) ) {
//        *pAddrOffset = 0;
//        return UNSUP_FILE;
//    }

//    *pAddrOffset = 0;
//    return SKIP_FILE;
//}

// take a look here: http://cs.nyu.edu/~gottlieb/courses/os/kholodov-fat.html
// to have info on fat file system
//int search_bin_file(uint8_t * root, uint8_t sector) {
//    // idx is a pointer inside the root dir
//    // we begin after all the existing entries
//    int idx = 0;
//    uint8_t found = 0;
//    uint32_t i = 0;
//    uint32_t move_sector_start = 0, nb_sector_to_move = 0;
//    FILE_TYPE file_type;
//    uint8_t hidden_file = 0, adapt_th_sector = 0;
//    uint32_t offset = 0;

//    FatDirectoryEntry_t* pDirEnts = (FatDirectoryEntry_t*)root;

//    if (sector == SECTORS_ROOT_IDX) {
//        // move past known existing files in the root dir
//        idx = (drag_success == 1) ? 12 : 13;
//    }

//    // first check that we did not receive any directory
//    // if we detect a directory -> disconnect / failed
//    for (i = idx; i < DIRENTS_PER_SECTOR; i++) {
//        if (pDirEnts[i].attributes & 0x10) {
//            reason = BAD_EXTENSION_FILE;
//            initDisconnect(0);
//            return -1;
//        }
//    }

//    // now do the real search for a valid .bin file
//    for (i = idx; i < DIRENTS_PER_SECTOR; i++) {

//        // Determine file type and get the flash offset
//        file_type = get_file_type(&pDirEnts[i], &offset);

//        if (file_type == BIN_FILE || file_type == PAR_FILE ||
//            file_type == DOW_FILE || file_type == CRD_FILE || file_type == SPI_FILE) {

//            hidden_file = (pDirEnts[i].attributes & 0x02) ? 1 : 0;

//            // compute the size of the file
//            size = pDirEnts[i].filesize;

//            if (size == 0) {
//              // skip empty files
//                continue;
//            }

//            // read the cluster number where data are stored (ignoring the
//            // two high bytes in the cluster number)
//            //
//            // Convert cluster number to sector number by moving past the root
//            // dir and fat tables.
//            //
//            // The cluster numbers start at 2 (0 and 1 are never used).
//            begin_sector = (pDirEnts[i].first_cluster_low_16 - 2) * WANTED_SECTORS_PER_CLUSTER + SECTORS_FIRST_FILE_IDX;

//            // compute the number of sectors
//            nb_sector = (size + MBR_BYTES_PER_SECTOR - 1) / MBR_BYTES_PER_SECTOR;

//            if ( (pDirEnts[i].filename[0] == '_') ||
//                 (pDirEnts[i].filename[0] == '.') ||
//                 (hidden_file && ((pDirEnts[i].filename[0] == '_') || (pDirEnts[i].filename[0] == '.'))) ||
//                 ((pDirEnts[i].filename[0] == 0xE5) && (file_type != CRD_FILE) && (file_type != PAR_FILE))) {
//                if (theoretical_start_sector == begin_sector) {
//                    adapt_th_sector = 1;
//                }
//                size = 0;
//                nb_sector = 0;
//                continue;
//            }

//            // if we receive a file with a valid extension
//            // but there has been program page error previously
//            // we fail / disconnect usb
//            if ((program_page_error == 1) && (maybe_erase == 0) && (start_sector >= begin_sector)) {
//                reason = RESERVED_BITS;
//                initDisconnect(0);
//                return -1;
//            }

//            adapt_th_sector = 0;

//            // on mac, with safari, we receive all the files with some more sectors at the beginning
//            // we have to move the sectors... -> 2x slower
//            if ((start_sector != 0) && (start_sector < begin_sector) && (current_sector - (begin_sector - start_sector) >= nb_sector)) {

//                // we need to copy all the sectors
//                // we don't listen to msd interrupt
//                listen_msc_isr = 0;

//                move_sector_start = (begin_sector - start_sector)*MBR_BYTES_PER_SECTOR;
//                nb_sector_to_move = (nb_sector % 2) ? nb_sector/2 + 1 : nb_sector/2;
//                for (i = 0; i < nb_sector_to_move; i++) {
//                    if (!swd_read_memory(move_sector_start + i*FLASH_SECTOR_SIZE, (uint8_t *)usb_buffer, FLASH_SECTOR_SIZE)) {
//                        failSWD();
//                        return -1;
//                    }
//                    if (!target_flash_erase_sector(i)) {
//                        failSWD();
//                        return -1;
//                    }
//                    if (!target_flash_program_page(i*FLASH_SECTOR_SIZE, (uint8_t *)usb_buffer, FLASH_SECTOR_SIZE)) {
//                        failSWD();
//                        return -1;
//                    }
//                }
//                initDisconnect(1);
//                return -1;
//            }

//            found = 1;
//            idx = i; // this is the file we want
//            good_file = 1;
//#if defined(BOARD_UBLOX_C027)
//            if (0 == memcmp((const char*)pDirEnts[i].filename, "~AUTORST", 8))
//                good_file = 2;
//            else if (0 == memcmp((const char*)pDirEnts[i].filename, "~AUTOCRP", 8))
//                good_file = 3;
//#endif
//            flash_addr_offset = offset;
//            break;
//        }
//        // if we receive a new file which does not have the good extension
//        // fail and disconnect usb
//        else if (file_type == UNSUP_FILE) {
//            reason = BAD_EXTENSION_FILE;
//            initDisconnect(0);
//            return -1;
//        }
//    }

//    if (adapt_th_sector) {
//        theoretical_start_sector += nb_sector;
//        init(0);
//    }
//    return (found == 1) ? idx : -1;
//}


//void usbd_msc_read_sect (uint32_t block, uint8_t *buf, uint32_t num_of_blocks) {
//    if (USBD_MSC_MediaReady) {
//        memcpy(buf, &DiskImage[block * USBD_MSC_BlockSize], num_of_blocks * USBD_MSC_BlockSize);
//    }
//    
////    if ((usb_state != USB_CONNECTED) || (listen_msc_isr == 0))
////        return;

////    if (USBD_MSC_MediaReady) {
////        // blink led not permanently
////        main_blink_msd_led(0);
////        memset(buf, 0, 512);

////        // Handle MBR, FAT1 sectors, FAT2 sectors, root1, root2 and mac file
////        if (block <= SECTORS_FIRST_FILE_IDX) {
////            memcpy(buf, sectors[block].sect, sectors[block].length);

////            // add new entry in FAT
////            if ((block == 1) && (drag_success == 0)) {
////                buf[9] = 0xff;
////                buf[10] = 0x0f;
////            } else if ((block == SECTORS_ROOT_IDX) && (drag_success == 0)) {
////                /* Appends a new directory entry at the end of the root file system.
////                    The entry is a copy of "fail[]" and the size is updated to match the
////                    length of the error reason string. The entry's set to point to cluster
////                    4 which is the first after the mbed.htm file."
////                */
////                memcpy(buf + sectors[block].length, fail, 16*2);
////                // adapt size of file according fail reason
////                buf[sectors[block].length + 28] = strlen((const char *)reason_array[reason]);
////                buf[sectors[block].length + 26] = 6;
////            }
////        }
//////        // send System Volume Information
//////        else if (block == SECTORS_SYSTEM_VOLUME_INFORMATION) {
//////            memcpy(buf, sect6, sizeof(sect6));
//////        }
//////        // send System Volume Information/IndexerVolumeGuid
//////        else if (block == SECTORS_INDEXER_VOLUME_GUID) {
//////            memcpy(buf, sect7, sizeof(sect7));
//////        }
////        // send mbed.html
////        else if (block == SECTORS_MBED_HTML_IDX) {
////            update_html_file();
////        }
////        // send error message file
////        else if (block == SECTORS_ERROR_FILE_IDX) {
////            memcpy(buf, reason_array[reason], strlen((const char *)reason_array[reason]));
////        }
////    }
//}

//static int programPage() {
//    //The timeout task's timer is resetted every 256kB that is flashed.
//    if ((flashPtr >= 0x40000) && ((flashPtr & 0x3ffff) == 0)) {
//        isr_evt_set(MSC_TIMEOUT_RESTART_EVENT, msc_valid_file_timeout_task_id);
//    }

//    // if we have received two sectors, write into flash
//    if (!target_flash_program_page(flashPtr + flash_addr_offset, (uint8_t *)usb_buffer, FLASH_PROGRAM_PAGE_SIZE)) {
//        // even if there is an error, adapt flashptr
//        flashPtr += FLASH_PROGRAM_PAGE_SIZE;
//        return 1;
//    }

//    // if we just wrote the last sector -> disconnect usb
//    if (current_sector == nb_sector) {
//        initDisconnect(1);
//        return 0;
//    }

//    flashPtr += FLASH_PROGRAM_PAGE_SIZE;

//    return 0;
//}


//void usbd_msc_write_sect (uint32_t block, uint8_t *buf, uint32_t num_of_blocks) {
//    if (USBD_MSC_MediaReady) {
//        memcpy(&Memory[block * USBD_MSC_BlockSize], buf, num_of_blocks * USBD_MSC_BlockSize);
//    }    
////    int idx_size = 0;

////    if ((usb_state != USB_CONNECTED) || (listen_msc_isr == 0))
////        return;

////    // we recieve the root directory
////    if ((block == SECTORS_ROOT_IDX) || (block == (SECTORS_ROOT_IDX+1))) {
////        // try to find a .bin file in the root directory
////        idx_size = search_bin_file(buf, block);

////        // .bin file exists
////        if (idx_size != -1) {

////            if (sector_received_first == 0) {
////                root_dir_received_first = 1;
////            }

////            // this means that we have received the sectors before root dir (linux)
////            // we have to flush the last page into the target flash
////            if ((sector_received_first == 1) && (current_sector == nb_sector) && (jtag_flash_init == 1)) {
////                if (msc_event_timeout == 0) {
////                    msc_event_timeout = 1;
////                    isr_evt_set(MSC_TIMEOUT_SPLIT_FILES_EVENT, msc_valid_file_timeout_task_id);
////                }
////                return;
////            }

////            // means that we are receiving additional sectors
////            // at the end of the file ===> we ignore them
////            if ((sector_received_first == 1) && (start_sector == begin_sector) && (current_sector > nb_sector) && (jtag_flash_init == 1)) {
////                initDisconnect(1);
////                return;
////            }
////        }
////    }
////    if (block >= SECTORS_ERROR_FILE_IDX) {

////        main_usb_busy_event();

////        if (root_dir_received_first == 0) {
////            sector_received_first = 1;
////        }

////        // if we don't receive consecutive sectors
////        // set maybe erase in case we receive other sectors
////        if ((previous_sector != 0) && ((previous_sector + 1) != block)) {
////            maybe_erase = 1;
////            return;
////        }

////        if (!flash_started && (block > theoretical_start_sector)) {
////            theoretical_start_sector = block;
////        }

////        // init jtag if needed
////        if (jtag_init() == 1) {
////            return;
////        }

////        if (jtag_flash_init == 1) {

////            main_blink_msd_led(1);

////            // We erase the chip if we received unrelated data before (mac compatibility)
////            if (maybe_erase && (block == theoretical_start_sector)) {
////                // avoid erasing the internal flash if only the external flash will be updated
////                if (flash_addr_offset == 0) {
////                    if (!target_flash_erase_chip()) {
////                    return;
////                    }
////                }
////                maybe_erase = 0;
////                program_page_error = 0;
////            }

////            // drop block < theoretical_sector
////            if (theoretical_start_sector > block) {
////                return;
////            }

////            if ((flash_started == 0) && (theoretical_start_sector == block)) {
////                flash_started = 1;
////                isr_evt_set(MSC_TIMEOUT_START_EVENT, msc_valid_file_timeout_task_id);
////                start_sector = block;
////            }

////            // at the beginning, we need theoretical_start_sector == block
////            if ((flash_started == 0) && (theoretical_start_sector != block)) {
////                reason = BAD_START_SECTOR;
////                initDisconnect(0);
////                return;
////            }

////            // not consecutive sectors detected
////            if ((flash_started == 1) && (maybe_erase == 0) && (start_sector != block) && (block != (start_sector + current_sector))) {
////                reason = NOT_CONSECUTIVE_SECTORS;
////                initDisconnect(0);
////                return;
////            }

////            // if we receive a new sector
////            // and the msc thread has been started (we thought that the file has been fully received)
////            // we kill the thread and write the sector in flash
////            if (msc_event_timeout == 1) {
////                msc_event_timeout = 0;
////            }

////            if (flash_started && (block == theoretical_start_sector)) {
////                // avoid erasing the internal flash if only the external flash will be updated
////                if (flash_addr_offset == 0) {
////                    if (!target_flash_erase_chip()) {
////                    return;
////                    }
////                }
////                maybe_erase = 0;
////                program_page_error = 0;
////            }

////            previous_sector = block;
////            current_sector++;
////            if (programPage() == 1) {
////                if (good_file) {
////                    reason = RESERVED_BITS;
////                    initDisconnect(0);
////                    return;
////                }
////                program_page_error = 1;
////                return;
////            }
////        }
////    }
//}


//void usbd_msc_init () {
//    USBD_MSC_MemorySize = 4096;
//    USBD_MSC_BlockSize  = 512;
//    USBD_MSC_BlockGroup = 1;
//    USBD_MSC_BlockCount = USBD_MSC_MemorySize / USBD_MSC_BlockSize;
//    USBD_MSC_BlockBuf   = BlockBuf;

//    //memcpy(Memory, DiskImage, 2048);
//    USBD_MSC_MediaReady = __TRUE;
////    if (!task_first_started) {
////        task_first_started = 1;
////        os_tsk_create_user(msc_valid_file_timeout_task, MSC_TASK_PRIORITY, msc_task_stack, MSC_TASK_STACK);
////    }

////    USBD_MSC_MemorySize = MBR_NUM_NEEDED_SECTORS * MBR_BYTES_PER_SECTOR;
////    USBD_MSC_BlockSize  = 512;
////    USBD_MSC_BlockGroup = 1;
////    USBD_MSC_BlockCount = USBD_MSC_MemorySize / USBD_MSC_BlockSize;
////    USBD_MSC_BlockBuf   = (uint8_t *)usb_buffer;
////    USBD_MSC_MediaReady = __TRUE;
//}
