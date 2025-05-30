/*
 * This file is part of GyroidOS
 * Copyright(c) 2013 - 2022 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <gyroidos@aisec.fraunhofer.de>
 */

#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <linux/ioctl.h>
#include <linux/unistd.h>
#include <linux/dm-ioctl.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/param.h>
#include <string.h>
#include <sys/mount.h>
#include <errno.h>
#include <linux/kdev_t.h>
#include <stdint.h>
#include <inttypes.h>

#include "cryptfs.h"
#include "macro.h"
#include "mem.h"
#include "proc.h"
#include "file.h"
#include "dm.h"
#include "fd.h"

#define TABLE_LOAD_RETRIES 10
#define INTEGRITY_TAG_SIZE 32
#define AUTHENC_KEY_LEN 96
#define CRYPTO_TYPE_AUTHENC "capi:authenc(hmac(sha256),xts(aes))-random"
#define CRYPTO_TYPE "aes-xts-plain64"
#define INTEGRITY_TYPE "hmac(sha256)"

#define CRYPTO_HEXKEY_LEN 2 * CRYPTFS_FDE_KEY_LEN
#define INTEGRITY_HEXKEY_LEN 2 * INTEGRITY_TAG_SIZE
#define AUTHENC_HEXKEY_LEN 2 * AUTHENC_KEY_LEN

/* taken from vold */
#define DEVMAPPER_BUFFER_SIZE 4096
#define DM_CRYPT_BUF_SIZE 4096
#define DM_INTEGRITY_BUF_SIZE 4096

#define ZERO_BUF_SIZE 100 * 1024 * 1024

/* FIXME Rejig library to record & use errno instead */
#ifndef DM_EXISTS_FLAG
#define DM_EXISTS_FLAG 0x00000004
#endif

/******************************************************************************/

static unsigned long
get_provided_data_sectors(const char *real_blk_name);

char *
cryptfs_get_device_path_new(const char *label)
{
	return mem_printf("%s/%s", DM_PATH_PREFIX, label);
}

#if 0
/* Convert a binary key of specified length into an ascii hex string equivalent,
 * without the leading 0x and with null termination
 */
static void
convert_key_to_hex_ascii(unsigned char *master_key, unsigned int keysize,
			      char *master_key_ascii)
{
	unsigned int i, a;
	unsigned char nibble;

	for (i = 0, a = 0; i < keysize; i++, a += 2) {
		/* For each byte, write out two ascii hex digits */
		nibble = (master_key[i] >> 4) & 0xf;
		master_key_ascii[a] = nibble + (nibble > 9 ? 0x37 : 0x30);

		nibble = master_key[i] & 0xf;
		master_key_ascii[a + 1] = nibble + (nibble > 9 ? 0x37 : 0x30);
	}

	/* Add the null termination */
	master_key_ascii[a] = '\0';

}
#endif

static int
load_integrity_mapping_table(int fd, const char *real_blk_name, const char *meta_blk_name,
			     const char *integrity_key_ascii, const char *name, int fs_size,
			     bool stacked)
{
	// General variables
	int ioctl_ret;

	IF_TRUE_RETVAL_ERROR(!stacked && !integrity_key_ascii, -1);

	// Load mapping variables
	char *mapping_buffer = mem_new(char, DM_INTEGRITY_BUF_SIZE);
	struct dm_target_spec *tgt;
	struct dm_ioctl *mapping_io;
	char *integrity_params;
	char *extra_params =
		stacked ? mem_printf("1 meta_device:%s", meta_blk_name) :
			  mem_printf("3 meta_device:%s internal_hash:%s:%s allow_discards",
				     meta_blk_name, INTEGRITY_TYPE, integrity_key_ascii);
	int mapping_counter;

	mapping_io = (struct dm_ioctl *)mapping_buffer;

	/* Load the mapping table for this device */
	tgt = (struct dm_target_spec *)&mapping_buffer[sizeof(struct dm_ioctl)];

	// Configure parameters for ioctl
	dm_ioctl_init(mapping_io, INDEX_DM_TABLE_LOAD, DM_INTEGRITY_BUF_SIZE, name, NULL, 0, 0, 0,
		      0);
	mapping_io->target_count = 1;
	tgt->status = 0;
	tgt->sector_start = 0;
	tgt->length = fs_size;
	strcpy(tgt->target_type, "integrity");

	// Write the intergity parameters at the end after dm_target_spec
	integrity_params = mapping_buffer + sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec);

	// Write parameter
	// these parameters are used in [1] as well as by dmsetup when traced with strace
	snprintf(integrity_params,
		 DM_INTEGRITY_BUF_SIZE - sizeof(struct dm_ioctl) - sizeof(struct dm_target_spec),
		 "%s 0 %d J %s", real_blk_name, INTEGRITY_TAG_SIZE, extra_params);

	mem_free0(extra_params);

	// Set pointer behind parameter
	integrity_params += strlen(integrity_params) + 1;
	// Byte align the parameter
	integrity_params = (char *)(((unsigned long)integrity_params + 7) &
				    ~8); /* Align to an 8 byte boundary */
	// Set tgt->next right behind dm_target_spec
	tgt->next = integrity_params - mapping_buffer;

	for (mapping_counter = 0; mapping_counter < TABLE_LOAD_RETRIES; mapping_counter++) {
		ioctl_ret = dm_ioctl(fd, DM_TABLE_LOAD, mapping_io);

		if (ioctl_ret == 0) {
			DEBUG("DM_TABLE_LOAD successfully returned %d", ioctl_ret);
			break;
		}
		NANOSLEEP(0, 500000000)
	}

	mem_free0(mapping_buffer);

	// Check that loading the table worked
	if (mapping_counter >= TABLE_LOAD_RETRIES) {
		ERROR_ERRNO("Loading integrity mapping table did not work after %d tries",
			    mapping_counter);
		return -1;
	}
	return mapping_counter + 1;
}

static int
load_crypto_mapping_table(int fd, const char *real_blk_name, const char *master_key_ascii,
			  const char *name, int fs_size, bool integrity)
{
	char *buffer = mem_new(char, DM_CRYPT_BUF_SIZE);
	struct dm_ioctl *io;
	struct dm_target_spec *tgt;
	char *crypt_params;
	char *extra_params = integrity ? mem_printf("1 integrity:%d:aead", INTEGRITY_TAG_SIZE) :
					 mem_printf("1 allow_discards");

	const char *crypto_type = integrity ? CRYPTO_TYPE_AUTHENC : CRYPTO_TYPE;

	int i;
	int ioctl_ret;

	TRACE("Loading crypto mapping table (%s,%s,%s,%s,%d,%d)", real_blk_name, crypto_type,
	      master_key_ascii, name, fs_size, fd);

	io = (struct dm_ioctl *)buffer;

	/* Load the mapping table for this device */
	tgt = (struct dm_target_spec *)&buffer[sizeof(struct dm_ioctl)];

	dm_ioctl_init(io, INDEX_DM_TABLE_LOAD, DM_CRYPT_BUF_SIZE, name, NULL, DM_EXISTS_FLAG, 0, 0,
		      0);
	io->target_count = 1;
	tgt->status = 0;
	tgt->sector_start = 0;
	tgt->length = fs_size;
	strcpy(tgt->target_type, "crypt");

	crypt_params = buffer + sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec);
	snprintf(crypt_params,
		 DM_CRYPT_BUF_SIZE - sizeof(struct dm_ioctl) - sizeof(struct dm_target_spec),
		 "%s %s 0 %s 0 %s", crypto_type, master_key_ascii, real_blk_name, extra_params);
	mem_free0(extra_params);

	crypt_params += strlen(crypt_params) + 1;
	crypt_params =
		(char *)(((unsigned long)crypt_params + 7) & ~8); /* Align to an 8 byte boundary */
	tgt->next = crypt_params - buffer;

	for (i = 0; i < TABLE_LOAD_RETRIES; i++) {
		ioctl_ret = dm_ioctl(fd, DM_TABLE_LOAD, io);
		if (!ioctl_ret) {
			DEBUG("Loading device table successfull.");
			break;
		}
		NANOSLEEP(0, 500000000)
	}

	mem_free0(buffer);
	if (i == TABLE_LOAD_RETRIES) {
		/* We failed to load the table, return an error */
		ERROR_ERRNO("Loading crypto mapping table did not work after %d tries", i);
		return -1;
	} else {
		return i + 1;
	}
}

static char *
create_device_node(const char *name)
{
	char *buffer = mem_new(char, DEVMAPPER_BUFFER_SIZE);
	char *device = NULL;

	int fd = open(DM_CONTROL, O_RDWR);
	if (fd < 0) {
		ERROR_ERRNO("Error opening devmapper");
		goto errout;
	}

	struct dm_ioctl *io = (struct dm_ioctl *)buffer;

	dm_ioctl_init(io, INDEX_DM_DEV_STATUS, DEVMAPPER_BUFFER_SIZE, name, NULL, 0, 0, 0, 0);
	if (dm_ioctl(fd, DM_DEV_STATUS, io)) {
		if (errno != ENXIO) {
			ERROR_ERRNO("DM_DEV_STATUS ioctl failed for lookup");
		}
		goto errout;
	}

	if (mkdir("/dev/block", 00777) < 0 && errno != EEXIST) {
		ERROR_ERRNO("Could not mkdir /dev/block");
		goto errout;
	}

	device = cryptfs_get_device_path_new(name);

	if (mknod(device, S_IFBLK | 00777, io->dev) != 0 && errno != EEXIST) {
		ERROR_ERRNO("Cannot mknod device %s", device);
		mem_free0(device);
	} else if (errno == EEXIST) {
		DEBUG("Device %s already exists, continuing", device);
	}

errout:

	close(fd);
	mem_free0(buffer);

	return device;
}

/**
 * Creates an integrity block device with DM_DEV_CREATE ioctl, reloads the mapping
 * table with DM_TABLE_LOADE ioctl and resumes the device with DM_DEV_SUSPEND ioctl
 *
 * [1] https://www.kernel.org/doc/html/latest/admin-guide/device-mapper/dm-integrity.html
 * [2] https://wiki.gentoo.org/wiki/Device-mapper#Integrity
 *
 * @return device name if successful, otherwise NULL
 */
static char *
create_integrity_blk_dev(const char *real_blk_name, const char *meta_blk_name, const char *key,
			 const char *name, const unsigned long fs_size, bool stacked)
{
	int fd;
	int ioctl_ret;
	int load_count = -1;
	char create_buffer[DM_INTEGRITY_BUF_SIZE];
	struct dm_ioctl *create_io;
	int create_counter;
	char *integrity_dev = NULL;

	// Open device mapper
	if ((fd = open(DM_CONTROL, O_RDWR)) < 0) {
		ERROR_ERRNO("Cannot open device-mapper");
		goto error;
	}

	// Create blk device
	// Initialize create_io struct
	create_io = (struct dm_ioctl *)create_buffer;
	dm_ioctl_init(create_io, INDEX_DM_DEV_CREATE, DM_INTEGRITY_BUF_SIZE, name, NULL, 0, 0, 0,
		      0);

	for (create_counter = 0; create_counter < TABLE_LOAD_RETRIES; create_counter++) {
		ioctl_ret = dm_ioctl(fd, DM_DEV_CREATE, create_io);
		if (!ioctl_ret) {
			DEBUG("Creating block device worked!");
			break;
		}

		NANOSLEEP(0, 500000000)
	}

	if (create_counter >= TABLE_LOAD_RETRIES) {
		ERROR_ERRNO("Failed to create block device after %d tries", create_counter);
		goto error;
	}

	// Load Integrity map table
	DEBUG("Loading Integrity mapping table");

	load_count = load_integrity_mapping_table(fd, real_blk_name, meta_blk_name, key, name,
						  fs_size, stacked);
	if (load_count < 0) {
		ERROR("Error while loading mapping table");
		goto error;
	} else {
		INFO("Loading integrity map took %d tries", load_count);
	}

	// Resume this device to activate it
	DEBUG("Resuming the blk device");
	dm_ioctl_init(create_io, INDEX_DM_DEV_SUSPEND, DM_INTEGRITY_BUF_SIZE, name, NULL, 0, 0, 0,
		      0);

	ioctl_ret = dm_ioctl(fd, DM_DEV_SUSPEND, create_io);
	if (ioctl_ret != 0) {
		ERROR_ERRNO("Cannot resume the dm-integrity device (ioctl ret: %d, errno:%d)",
			    ioctl_ret, errno);
		goto error;
	}

	integrity_dev = create_device_node(name);
	if (!integrity_dev) {
		ERROR("Could not create device node (integrity)");
		goto error;
	} else {
		DEBUG("Successfully created device node (integrity)");
	}

	close(fd);
	return integrity_dev;

error:
	ERROR("Failed integrity block creation");
	close(fd);
	return NULL;
}

static char *
create_crypto_blk_dev(const char *real_blk_name, const char *master_key, const char *name,
		      unsigned long fs_size, bool integrity)
{
	char buffer[DM_CRYPT_BUF_SIZE];
	struct dm_ioctl *io;
	int fd;
	int load_count;
	int i;
	int ioctl_ret;
	char *crypto_blkdev = NULL;

	DEBUG("Creating crypto blk device");
	if ((fd = open(DM_CONTROL, O_RDWR)) < 0) {
		ERROR("Cannot open device-mapper\n");
		goto error;
	}

	io = (struct dm_ioctl *)buffer;
	dm_ioctl_init(io, INDEX_DM_DEV_CREATE, DM_CRYPT_BUF_SIZE, name, NULL, 0, 0, 0, 0);

	for (i = 0; i < TABLE_LOAD_RETRIES; i++) {
		ioctl_ret = dm_ioctl(fd, DM_DEV_CREATE, io);

		if (!ioctl_ret) {
			DEBUG("Cryptp DM_DEV_CREATE worked!");
			break;
		}
		NANOSLEEP(0, 500000000)
	}

	if (i == TABLE_LOAD_RETRIES) {
		/* We failed to load the table, return an error */
		ERROR("Cannot create dm-crypt device");
		goto error;
	}

	load_count =
		load_crypto_mapping_table(fd, real_blk_name, master_key, name, fs_size, integrity);
	if (load_count < 0) {
		ERROR("Cannot load dm-crypt mapping table");
		goto error;
	} else if (load_count > 1) {
		INFO("Took %d tries to load dmcrypt table.\n", load_count);
	}

	/* Resume this device to activate it */
	dm_ioctl_init(io, INDEX_DM_DEV_SUSPEND, DM_CRYPT_BUF_SIZE, name, NULL, 0, 0, 0, 0);

	if (dm_ioctl(fd, DM_DEV_SUSPEND, io)) {
		ERROR_ERRNO("Cannot resume the dm-crypt device\n");
		goto error;
	}

	crypto_blkdev = create_device_node(name);
	if (!crypto_blkdev) {
		ERROR("Could not create device node (crypt)");
		goto error;
	} else {
		DEBUG("Successfully created device node (crypt)");
	}

	close(fd);
	return crypto_blkdev;

error:
	close(fd); /* If fd is <0 from a failed open call, it's safe to just ignore the close error */
	ERROR("Failed crypto block creation wiht name '%s'", name);
	return NULL;
}

static int
delete_integrity_blk_dev(const char *name)
{
	int fd;
	char *buffer = mem_new(char, DEVMAPPER_BUFFER_SIZE);
	struct dm_ioctl *io;
	int ret = -1;
	char *device = NULL;

	fd = open(DM_CONTROL, O_RDWR);
	if (fd < 0) {
		ERROR_ERRNO("Cannot open device-mapper");
		goto error;
	}

	io = (struct dm_ioctl *)buffer;

	dm_ioctl_init(io, INDEX_DM_DEV_REMOVE, DM_INTEGRITY_BUF_SIZE, name, NULL, 0, 0, 0, 0);
	if (dm_ioctl(fd, DM_DEV_REMOVE, io) < 0) {
		ret = errno;
		if (errno != ENXIO)
			ERROR_ERRNO("Cannot remove dm-integrity device '%s'", name);
		goto error;
	}

	/* remove device node if necessary */
	device = cryptfs_get_device_path_new(name);
	unlink(device);

	DEBUG("Successfully deleted dm-integrity device '%s'", name);
	ret = 0;

error:
	if (device)
		mem_free0(device);
	mem_free(buffer);
	close(fd);
	return ret;
}

static int
delete_crypto_blk_dev(int fd, const char *name)
{
	char *buffer = mem_new(char, DM_CRYPT_BUF_SIZE);
	struct dm_ioctl *io;
	int ret = -1;
	char *device = NULL;
	bool internally_opend = false;

	if (fd == -1) {
		fd = open(DM_CONTROL, O_RDWR);
		internally_opend = true;
	}
	if (fd < 0) {
		ERROR_ERRNO("Cannot open device-mapper");
		goto error;
	}

	io = (struct dm_ioctl *)buffer;

	dm_ioctl_init(io, INDEX_DM_DEV_REMOVE, DM_CRYPT_BUF_SIZE, name, NULL, 0, 0, 0, 0);
	if (dm_ioctl(fd, DM_DEV_REMOVE, io) < 0) {
		ret = errno;
		if (errno != ENXIO)
			ERROR_ERRNO("Cannot remove dm-crypt device '%s'", name);
		goto error;
	}

	/* remove device node if necessary */
	device = cryptfs_get_device_path_new(name);
	unlink(device);
	mem_free0(device);

	DEBUG("Successfully deleted dm-crypt device '%s'", name);
	ret = 0;

error:
	if (device)
		mem_free0(device);
	mem_free(buffer);
	if (internally_opend)
		close(fd);
	return ret;
}

static unsigned long
get_provided_data_sectors(const char *real_blk_name)
{
	int fd;
	unsigned long provided_data_sectors = 0;
	char magic[8]; // "integrt" on a valid superblock

	if ((fd = open(real_blk_name, O_RDONLY)) < 0) {
		ERROR("Cannot open volume %s", real_blk_name);
		return 0;
	}

	int bytes_read = read(fd, magic, sizeof(magic));
	DEBUG("Bytes read: %d, '%s'", bytes_read, magic);
	if (bytes_read != sizeof(magic)) {
		ERROR("Cannot read superblock type from volume %s", real_blk_name);
		goto errout;
	}
	if (strcmp(magic, "integrt") != 0) {
		DEBUG("No existing integrity superblock detected on %s", real_blk_name);
		provided_data_sectors = 1;
		goto errout;
	}

	// 16 Bytes offset from start of superblock for provided_data_sectors
	lseek(fd, 16, SEEK_SET);
	bytes_read = read(fd, &provided_data_sectors, sizeof(provided_data_sectors));
	DEBUG("Read bytes is: %d", bytes_read);

	if (bytes_read != sizeof(provided_data_sectors) || provided_data_sectors == 0) {
		ERROR("Cannot read provided_data_sectors from volume %s", real_blk_name);
		goto errout;
	}

errout:
	DEBUG("Returning: provided_data_sectors= %ld", provided_data_sectors);
	close(fd);
	return provided_data_sectors;
}

static int
cryptfs_write_zeros(char *crypto_blkdev, size_t size)
{
	int fd = -1, ret = -1;
	char *zeros = NULL;
	ssize_t towrite = -1, res = -1, written = 0;

	IF_NULL_RETVAL(crypto_blkdev, -1);

	if (!(zeros = calloc(1, ZERO_BUF_SIZE))) {
		ERROR_ERRNO("Failed to allocate zero buffer with size %zd", size);
		goto error;
	}

	if (0 > (fd = open(crypto_blkdev, O_WRONLY))) {
		ERROR("Cannot open volume %s", crypto_blkdev);
		goto error;
	}

	while (0 < (size - written)) {
		towrite = MIN(size - written, ZERO_BUF_SIZE);

		if (0 > (res = fd_write(fd, zeros, towrite))) {
			ERROR("Failed to write: %zd bytes", towrite);
			goto error;
		}
		TRACE("res: %zd, written %zd, towrite %zd\n", res, written, towrite);

		written += res;
	}

	INFO("Syncing volume '%s' to disk after MAC generation", crypto_blkdev);
	if (0 != fsync(fd)) {
		ERROR_ERRNO("Failed to sync fd %d", fd);
		goto error;
	}
	INFO("Successfully generated initial MACs on volume '%s'", crypto_blkdev);

	ret = 0;

error:
	if (0 <= fd)
		close(fd);

	if (zeros)
		free(zeros);

	return ret;
}

char *
cryptfs_setup_volume_new(const char *label, const char *real_blkdev, const char *key,
			 const char *meta_blkdev, cryptfs_mode_t mode)
{
	int fd;
	// The file system size in sectors
	uint64_t fs_size;

	bool encrypt, integrity, stacked;

	char *crypto_blkdev = NULL, *integrity_blkdev = NULL;
	char *crypto_key = NULL, *integrity_key = NULL;
	size_t crypto_key_len = 0, integrity_key_len = 0;
	char *integrity_dev_label = NULL;
	bool initial_format = false;

	/* do parameter validation */
	IF_NULL_RETVAL_ERROR(label, NULL);
	IF_NULL_RETVAL_ERROR(real_blkdev, NULL);
	IF_NULL_RETVAL_ERROR(key, NULL);

	switch (mode) {
	case CRYPTFS_MODE_NOT_IMPLEMENTED:
		WARN("cyrptfs mode NOT_IMPLEMENTED! just returning real_blkdev %s!", real_blkdev);
		return mem_strdup(real_blkdev);
	case CRYPTFS_MODE_AUTHENC:
		IF_NULL_RETVAL_ERROR(meta_blkdev, NULL);
		crypto_key_len = strlen(key);
		if (strlen(key) != AUTHENC_HEXKEY_LEN) {
			WARN("strlen(key) != AUTHENC_HEXKEY_LEN [%d]) using len=%zu",
			     AUTHENC_HEXKEY_LEN, crypto_key_len);
		}
		integrity_key_len = 0;
		encrypt = true;
		integrity = true;
		stacked = true;
		break;
	case CRYPTFS_MODE_ENCRYPT_ONLY:
		crypto_key_len = strlen(key);
		if (strlen(key) != CRYPTO_HEXKEY_LEN) {
			WARN("strlen(key) != CRYPTO_HEXKEY_LEN [%d]) using len=%zu",
			     CRYPTO_HEXKEY_LEN, crypto_key_len);
		}
		integrity_key_len = 0;
		encrypt = true;
		integrity = false;
		stacked = false;
		break;
	case CRYPTFS_MODE_INTEGRITY_ENCRYPT:
		IF_NULL_RETVAL_ERROR(meta_blkdev, NULL);
		IF_TRUE_RETVAL(strlen(key) != CRYPTO_HEXKEY_LEN + INTEGRITY_HEXKEY_LEN, NULL);
		crypto_key_len = CRYPTO_HEXKEY_LEN;
		integrity_key_len = INTEGRITY_HEXKEY_LEN;
		encrypt = true;
		integrity = true;
		stacked = false;
		break;
	case CRYPTFS_MODE_INTEGRITY_ONLY:
		IF_TRUE_RETVAL(strlen(key) != INTEGRITY_HEXKEY_LEN, NULL);
		crypto_key_len = 0;
		integrity_key_len = INTEGRITY_HEXKEY_LEN;
		encrypt = false;
		integrity = true;
		stacked = false;
		break;
	default:
		ERROR("Unsupported type and or parameter combination");
		return NULL;
	}
	/* validated parameters */

	/*
	 * Use the first 128 hex digits (64 byte) of master key for 512 bit xts mode
	 * or full master key for AUTHENC mode
	 */
	crypto_key = crypto_key_len > 0 ? mem_strndup(key, crypto_key_len) : NULL;

	/* Use the following 64 hex digits (32 byte) of master key for 256 bit hmac */
	integrity_key =
		integrity_key_len > 0 ? mem_strndup(key + crypto_key_len, integrity_key_len) : NULL;

	//DEBUG("KEYS:\n\t key:\t %s\n\t split:\t %s:%s", key, crypto_key ? crypto_key : "(null)",
	//      integrity_key ? integrity_key : "(null)");

	/* Update the fs_size field to be the size of the volume */
	if ((fd = open(real_blkdev, O_RDONLY)) < 0) {
		ERROR("Cannot open volume %s", real_blkdev);
		goto error;
	}
	// BLKGETSIZE64 returns size in bytes, we require size in sectors
	int sector_size = dm_get_blkdev_sector_size(fd);
	if (!(sector_size > 0)) {
		ERROR("dm_get_blkdev_sector_size returned %d\n", sector_size);
		close(fd);
		goto error;
	}
	fs_size = dm_get_blkdev_size64(fd) / sector_size;
	close(fd);

	if (fs_size == 0) {
		ERROR("Cannot get size of volume %s", real_blkdev);
		goto error;
	}
	DEBUG("Crypto blk device size: %" PRIu64, fs_size);

	if (integrity) {
		integrity_dev_label = mem_printf("%s-%s", label, "integrity");
		TRACE("Going to create integrity blk dev with label: %s", integrity_dev_label);

		/* check if meta device is initialized */
		initial_format = get_provided_data_sectors(meta_blkdev) != fs_size;

		if (!(integrity_blkdev =
			      create_integrity_blk_dev(real_blkdev, meta_blkdev, integrity_key,
						       integrity_dev_label, fs_size, stacked))) {
			ERROR("create_integrity_blk_dev '%s' failed!", integrity_dev_label);
			goto error;
		}
	}

	if (encrypt) {
		if (!(crypto_blkdev =
			      create_crypto_blk_dev(meta_blkdev ? integrity_blkdev : real_blkdev,
						    crypto_key, label, fs_size, stacked))) {
			ERROR("Could not create crypto block device");
			goto error;
		}
	} else {
		crypto_blkdev = integrity_blkdev;
	}

	if (initial_format) {
		/*
		 * format crypto device, otherwise I/O errors may occur
		 * also during write attempts which are not bound to
		 * sector/block size for which no integrity data exist yet.
		 * This is due to the block has to be read first than.
		 */
		DEBUG("Formatting crypto blkdev %s. Generating initial MAC on "
		      "integrity blkdev %s",
		      crypto_blkdev, integrity_blkdev);

		if (0 != cryptfs_write_zeros(crypto_blkdev, fs_size * 512)) {
			WARN("Failed to format volume %s using calloc, falling back to stack-allocated buffer",
			     crypto_blkdev);

			int fd;
			if ((fd = open(crypto_blkdev, O_WRONLY | O_DIRECT)) < 0) {
				ERROR("Cannot open volume %s", crypto_blkdev);
				goto error;
			}

			char zeros[DM_INTEGRITY_BUF_SIZE] __attribute__((__aligned__(512))) = { 0 };
			for (unsigned long i = 0; i < fs_size / 8; ++i) {
				if (write(fd, zeros, DM_INTEGRITY_BUF_SIZE) <
				    DM_INTEGRITY_BUF_SIZE) {
					ERROR_ERRNO("Could not write empty block %lu to %s", i,
						    crypto_blkdev);
					close(fd);
					goto error;
				}
			}
			close(fd);

			DEBUG("Successfully formatted volume %s using file_copy", crypto_blkdev);
		}
	}

	if (crypto_key) {
		mem_memset0(crypto_key, crypto_key_len);
		mem_free0(crypto_key);
	}
	if (integrity_key) {
		mem_memset0(integrity_key, integrity_key_len);
		mem_free0(integrity_key);
	}
	if (integrity_dev_label)
		mem_free0(integrity_dev_label);
	if (integrity_blkdev && integrity_blkdev != crypto_blkdev)
		mem_free0(integrity_blkdev);

	return crypto_blkdev;

error:
	if (crypto_key) {
		mem_memset0(crypto_key, crypto_key_len);
		mem_free0(crypto_key);
	}
	if (integrity_key) {
		mem_memset0(integrity_key, integrity_key_len);
		mem_free0(integrity_key);
	}
	if (integrity_dev_label)
		mem_free0(integrity_dev_label);
	if (integrity_blkdev) {
		delete_integrity_blk_dev(label);
		mem_free0(integrity_blkdev);
	}
	if (crypto_blkdev) {
		delete_crypto_blk_dev(-1, label);
		mem_free0(crypto_blkdev);
	}

	return NULL;
}

int
cryptfs_delete_blk_dev(int fd, const char *name, cryptfs_mode_t mode)
{
	bool encrypt, integrity;

	switch (mode) {
	case CRYPTFS_MODE_AUTHENC:
	case CRYPTFS_MODE_INTEGRITY_ENCRYPT:
		encrypt = true;
		integrity = true;
		break;
	case CRYPTFS_MODE_ENCRYPT_ONLY:
		encrypt = true;
		integrity = false;
		break;
	case CRYPTFS_MODE_INTEGRITY_ONLY:
		encrypt = false;
		integrity = true;
		break;
	default:
		ERROR("Unsupported mode.");
		return -1;
	}

	if (encrypt) {
		if (delete_crypto_blk_dev(fd, name) < 0) {
			ERROR("Failed to delete crypto dev: %s", name);
			return -1;
		}
	}

	if (integrity) {
		char *integrity_dev_name = mem_printf("%s-%s", name, "integrity");
		if (delete_integrity_blk_dev(integrity_dev_name) < 0) {
			ERROR("Failed to delete integrity dev: %s", integrity_dev_name);
			mem_free0(integrity_dev_name);
			return -1;
		}
		mem_free0(integrity_dev_name);
	}

	return 0;
}
