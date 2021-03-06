// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define LONGNAME_MAXLENGTH 512
#define LONGNAME_MAXCHUNKS 40

#define DEBUG_PRINT(...) printf(__VA_ARGS__)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";

int isFAT32(struct fat_boot_header fb);

static void
vfat_init(const char *dev)
{
    struct fat_boot_header s;

    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &vfat_info.fb, sizeof(vfat_info.fb), 0) != sizeof(s))
        err(1, "read super block");
	if(!isFAT32(vfat_info.fb)) {
		err(1, "%s is not a FAT32 system\n", dev);
	}
    
    /* Point 2: parse the BPB sector info */
	vfat_info.fat_begin_offset = vfat_info.fb.reserved_sectors * vfat_info.fb.bytes_per_sector;
	vfat_info.cluster_begin_offset = vfat_info.fat_begin_offset + (vfat_info.fb.sectors_per_fat * vfat_info.fb.bytes_per_sector * vfat_info.fb.fat_count);
	vfat_info.cluster_size = vfat_info.fb.sectors_per_cluster * vfat_info.fb.bytes_per_sector;
    vfat_info.bytes_per_sector = vfat_info.fb.bytes_per_sector;
    vfat_info.sectors_per_cluster = vfat_info.fb.sectors_per_cluster;
    vfat_info.reserved_sectors = vfat_info.fb.reserved_sectors;
    vfat_info.fat_size = vfat_info.fb.sectors_per_fat * vfat_info.fb.bytes_per_sector;
    vfat_info.fat_entries = vfat_info.fat_size/sizeof(uint32_t);

	vfat_info.fat = mmap_file(vfat_info.fd, vfat_info.fat_begin_offset, 512 /*todo: size??*/); 

    /* XXX add your code here */
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_mode = 0555 | S_IFDIR;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;

}

/* Point 1: read the first 512 bytes from the drive */
int vfat_read_from_file (void *bootSector, char *fileName)
{
  FILE *fileHandle;
  int result;

  fileHandle = fopen(fileName, "rb");
  if (fileHandle == NULL) {
    DEBUG_PRINT("ERROR: Error opening file: %s", fileName);
    return 0;
  }

  result = fread(bootSector, 1, 512, fileHandle);
  if (result != 512) {
    DEBUG_PRINT("ERROR: Error reading file: %s", fileName);
    return 0;
  }

  fclose(fileHandle);
  return result;
}

/* Point 2: check is the drive is FAT32 */
int isFAT32(struct fat_boot_header fb) {
	int root_dir_sectors = (fb.root_max_entries*32 + (fb.bytes_per_sector - 1)) / fb.bytes_per_sector;
	
	if(root_dir_sectors != 0) {
		return 0;
	}
	
	uint32_t FATSz;
	uint32_t totSec;
	uint32_t dataSec;
	uint32_t countOfClusters;
	
	if(fb.sectors_per_fat_small != 0) {
		FATSz = fb.sectors_per_fat_small;
	}
    else {
		FATSz = fb.sectors_per_fat;
	}
	
	if(fb.total_sectors_small != 0) {
		totSec = fb.total_sectors_small;
	} 
    else {
		totSec = fb.total_sectors;
	}
	
	dataSec = totSec - (fb.reserved_sectors + (fb.fat_count * FATSz) + root_dir_sectors);
	
	countOfClusters = dataSec / fb.sectors_per_cluster;

    if (countOfClusters >= 65525) {
        return 1;
    }
    else {
        return 0;
    }
}

int vfat_next_cluster(uint32_t c)
{

    /* Point 2: Read FAT to actually get the next cluster */
	u_int32_t next_cluster = vfat_info.fat[c];
		
    return next_cluster;
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata)
{
    struct stat st; // we can reuse same stat entry over and over again

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    /* XXX add your code here */
    return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    off_t first_cluster;
    const char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
    struct vfat_search_data *sd = data;

    if (strcmp(sd->name, name) != 0) return 0;

    sd->found = 1;
    *sd->st = *st;

    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
*/
int vfat_resolve(const char *path, struct stat *st)
{
    /* TODO: Add your code here.
        You should tokenize the path (by slash separator) and then
        for each token search the directory for the file/dir with that name.
        You may find it useful to use following functions:
        - strtok to tokenize by slash. See manpage
        - vfat_readdir in conjuction with vfat_search_entry
    */
    struct vfat_search_data sd;
	sd.st = st;
	uint32_t current_cluster = vfat_info.fb.root_cluster;

    const char separator[2] = "/";
	
	if (strcmp(separator, path) == 0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRUSR | S_IRGRP | S_IROTH | S_IFDIR;
		st->st_nlink = 1;
		st->st_uid = vfat_info.mount_uid;
		st->st_gid = vfat_info.mount_gid;
		st->st_rdev = 0;
		st->st_size = 0;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return current_cluster;
	}
    else {
		char path_copy[LONGNAME_MAXLENGTH];
		strcpy(path_copy, path);
		
		char* token;
		token = strtok(path_copy, separator);
		
		while(token != NULL) {
			sd.first_cluster = 0;
			sd.found = 0;
			sd.name = token;
			
			vfat_readdir(current_cluster, vfat_search_entry, &sd);
			current_cluster = sd.first_cluster;
			
			if(sd.found == 0) {
				return -ENOENT;
			}
			
			token = strtok(NULL, separator);
		}
	}
	
	return current_cluster;
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
}

// Extended attributes useful for debugging
int vfat_fuse_getxattr(const char *path, const char* name, char* buf, size_t size)
{
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
        const char *path, void *callback_data,
        fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
    */
    return 0;
}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    /* TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
    */
    return 0;
}

////////////// No need to modify anything below this point
int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
    .getattr = vfat_fuse_getattr,
    .getxattr = vfat_fuse_getxattr,
    .readdir = vfat_fuse_readdir,
    .read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
