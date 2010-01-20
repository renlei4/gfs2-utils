#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <errno.h>

#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "fs_recovery.h"

#define CLEAR_POINTER(x) \
	if(x) { \
		free(x); \
		x = NULL; \
	}

/**
 * block_mounters
 *
 * Change the lock protocol so nobody can mount the fs
 *
 */
static int block_mounters(struct gfs2_sbd *sbp, int block_em)
{
	if(block_em) {
		/* verify it starts with lock_ */
		if(!strncmp(sbp->sd_sb.sb_lockproto, "lock_", 5)) {
			/* Change lock_ to fsck_ */
			memcpy(sbp->sd_sb.sb_lockproto, "fsck_", 5);
		}
		/* FIXME: Need to do other verification in the else
		 * case */
	} else {
		/* verify it starts with fsck_ */
		/* verify it starts with lock_ */
		if(!strncmp(sbp->sd_sb.sb_lockproto, "fsck_", 5)) {
			/* Change fsck_ to lock_ */
			memcpy(sbp->sd_sb.sb_lockproto, "lock_", 5);
		}
	}

	if(write_sb(sbp)) {
		stack;
		return -1;
	}
	return 0;
}

static void gfs2_dup_free(void)
{
	struct dup_blks *f;

	while(!osi_list_empty(&dup_blocks.list)) {
		f = osi_list_entry(dup_blocks.list.next, struct dup_blks,
				   list);
		while (!osi_list_empty(&f->ref_inode_list))
			osi_list_del(&f->ref_inode_list);
		osi_list_del(&f->list);
		free(f);
	}
}

/*
 * empty_super_block - free all structures in the super block
 * sdp: the in-core super block
 *
 * This function frees all allocated structures within the
 * super block.  It does not free the super block itself.
 *
 * Returns: Nothing
 */
static void empty_super_block(struct gfs2_sbd *sdp)
{
	uint32_t i;

	log_info( _("Freeing buffers.\n"));
	gfs2_rgrp_free(&sdp->rglist);

	for(i = 0; i < FSCK_HASH_SIZE; i++) {
		while(!osi_list_empty(&inode_hash[i])) {
			struct inode_info *ii;
			ii = osi_list_entry(inode_hash[i].next, struct inode_info, list);
			osi_list_del(&ii->list);
			free(ii);
		}
		while(!osi_list_empty(&dir_hash[i])) {
			struct dir_info *di;
			di = osi_list_entry(dir_hash[i].next, struct dir_info, list);
			osi_list_del(&di->list);
			free(di);
		}
	}

	if (bl) {
		gfs2_bmap_destroy(sdp, bl);
		gfs2_dup_free();
	}
}


/**
 * set_block_ranges
 * @sdp: superblock
 *
 * Uses info in rgrps and jindex to determine boundaries of the
 * file system.
 *
 * Returns: 0 on success, -1 on failure
 */
static int set_block_ranges(struct gfs2_sbd *sdp)
{

	struct rgrp_list *rgd;
	struct gfs2_rindex *ri;
	osi_list_t *tmp;
	char buf[sdp->sd_sb.sb_bsize];
	uint64_t rmax = 0;
	uint64_t rmin = 0;
	int error;

	log_info( _("Setting block ranges...\n"));

	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next)
	{
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		ri = &rgd->ri;
		if (ri->ri_data0 + ri->ri_data - 1 > rmax)
			rmax = ri->ri_data0 + ri->ri_data - 1;
		if (!rmin || ri->ri_data0 < rmin)
			rmin = ri->ri_data0;
	}

	last_fs_block = rmax;
	if (last_fs_block > 0xffffffff && sizeof(unsigned long) <= 4) {
		log_crit( _("This file system is too big for this computer to handle.\n"));
		log_crit( _("Last fs block = 0x%llx, but sizeof(unsigned long) is %zu bytes.\n"),
			 (unsigned long long)last_fs_block,
			 sizeof(unsigned long));
		goto fail;
	}

	last_data_block = rmax;
	first_data_block = rmin;

	if(fsck_lseek(sdp->device_fd, (last_fs_block * sdp->sd_sb.sb_bsize))){
		log_crit( _("Can't seek to last block in file system: %"
				 PRIu64" (0x%" PRIx64 ")\n"), last_fs_block, last_fs_block);
		goto fail;
	}

	memset(buf, 0, sdp->sd_sb.sb_bsize);
	error = read(sdp->device_fd, buf, sdp->sd_sb.sb_bsize);
	if (error != sdp->sd_sb.sb_bsize){
		log_crit( _("Can't read last block in file system (error %u), "
				 "last_fs_block: %"PRIu64" (0x%" PRIx64 ")\n"), error,
				 last_fs_block, last_fs_block);
		goto fail;
	}

	return 0;

 fail:
	return -1;
}

/**
 * check_rgrp_integrity - verify a rgrp free block count against the bitmap
 */
static void check_rgrp_integrity(struct gfs2_sbd *sdp, struct rgrp_list *rgd,
				 int *fixit, int *this_rg_fixed,
				 int *this_rg_bad)
{
	uint32_t rg_free, rg_reclaimed;
	int rgb, x, y, off, bytes_to_check, total_bytes_to_check;
	unsigned int state;

	rg_free = rg_reclaimed = 0;
	total_bytes_to_check = rgd->ri.ri_bitbytes;
	*this_rg_fixed = *this_rg_bad = 0;

	for (rgb = 0; rgb < rgd->ri.ri_length; rgb++){
		/* Count up the free blocks in the bitmap */
		off = (rgb) ? sizeof(struct gfs2_meta_header) :
			sizeof(struct gfs2_rgrp);
		if (total_bytes_to_check <= sdp->bsize - off)
			bytes_to_check = total_bytes_to_check;
		else
			bytes_to_check = sdp->bsize - off;
		total_bytes_to_check -= bytes_to_check;
		for (x = 0; x < bytes_to_check; x++) {
			unsigned char *byte;

			byte = (unsigned char *)&rgd->bh[rgb]->b_data[off + x];
			if (*byte == 0x55)
				continue;
			if (*byte == 0x00) {
				rg_free += GFS2_NBBY;
				continue;
			}
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (*byte >>
					 (GFS2_BIT_SIZE * y)) & GFS2_BIT_MASK;
				if (state == GFS2_BLKST_USED)
					continue;
				if (state == GFS2_BLKST_DINODE)
					continue;
				if (state == GFS2_BLKST_FREE) {
					rg_free++;
					continue;
				}
				/* GFS2_BLKST_UNLINKED */
				*this_rg_bad = 1;
				if (!(*fixit)) {
					if (query(_("Okay to reclaim unlinked "
						    "inodes? (y/n)")))
						*fixit = 1;
				}
				if (!(*fixit))
					continue;
				*byte &= ~(GFS2_BIT_MASK <<
					   (GFS2_BIT_SIZE * y));
				bmodified(rgd->bh[rgb]);
				rg_reclaimed++;
				rg_free++;
				*this_rg_fixed = 1;
			}
		}
	}
	if (rgd->rg.rg_free != rg_free) {
		*this_rg_bad = 1;
		log_err( _("Error: resource group %lld (0x%llx): "
			   "free space (%d) does not match bitmap (%d)\n"),
			 (unsigned long long)rgd->ri.ri_addr,
			 (unsigned long long)rgd->ri.ri_addr,
			 rgd->rg.rg_free, rg_free);
		if (rg_reclaimed)
			log_err( _("(%d blocks were reclaimed)\n"),
				 rg_reclaimed);
		if (query( _("Fix the rgrp free blocks count? (y/n)"))) {
			rgd->rg.rg_free = rg_free;
			gfs2_rgrp_out(&rgd->rg, rgd->bh[0]);
			*this_rg_fixed = 1;
			log_err( _("The rgrp was fixed.\n"));
		} else
			log_err( _("The rgrp was not fixed.\n"));
	}
	/*
	else {
		log_debug( _("Resource group %lld (0x%llx) free space "
			     "is consistent: free: %d reclaimed: %d\n"),
			   (unsigned long long)rgd->ri.ri_addr,
			   (unsigned long long)rgd->ri.ri_addr,
			   rg_free, rg_reclaimed);
	}*/
}

/**
 * check_rgrps_integrity - verify rgrp consistency
 *
 * Returns: 0 on success, 1 if errors were detected
 */
static int check_rgrps_integrity(struct gfs2_sbd *sdp)
{
	int rgs_good = 0, rgs_bad = 0, rgs_fixed = 0;
	int was_bad = 0, was_fixed = 0, error = 0;
	osi_list_t *tmp;
	struct rgrp_list *rgd;
	int reclaim_unlinked = 0;

	log_info( _("Checking the integrity of all resource groups.\n"));
	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		if (fsck_abort)
			return 0;
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		check_rgrp_integrity(sdp, rgd, &reclaim_unlinked,
				     &was_fixed, &was_bad);
		if (was_fixed)
			rgs_fixed++;
		if (was_bad) {
			error = 1;
			rgs_bad++;
		} else
			rgs_good++;
	}
	if (rgs_bad)
		log_err( _("RGs: Consistent: %d   Inconsistent: %d   Fixed: %d"
			   "   Total: %d\n"),
			 rgs_good, rgs_bad, rgs_fixed, rgs_good + rgs_bad);
	return error;
}

/**
 * init_system_inodes
 *
 * Returns: 0 on success, -1 on failure
 */
static int init_system_inodes(struct gfs2_sbd *sdp)
{
	uint64_t inumbuf;
	char *buf;
	struct gfs2_statfs_change sc;
	int rgcount;
	enum rgindex_trust_level trust_lvl;
	uint64_t addl_mem_needed;

	/*******************************************************************
	 ******************  Initialize important inodes  ******************
	 *******************************************************************/

	log_info( _("Initializing special inodes...\n"));

	/* Get master dinode */
	sdp->master_dir = inode_read(sdp, sdp->sd_sb.sb_master_dir.no_addr);
	/* Get root dinode */
	sdp->md.rooti = inode_read(sdp, sdp->sd_sb.sb_root_dir.no_addr);

	/* Look for "inum" entry in master dinode */
	gfs2_lookupi(sdp->master_dir, "inum", 4, &sdp->md.inum);
	/* Read inum entry into buffer */
	gfs2_readi(sdp->md.inum, &inumbuf, 0, sdp->md.inum->i_di.di_size);
	/* call gfs2_inum_range_in() to retrieve range */
	sdp->md.next_inum = be64_to_cpu(inumbuf);

	gfs2_lookupi(sdp->master_dir, "statfs", 6, &sdp->md.statfs);
	buf = malloc(sdp->md.statfs->i_di.di_size);
	// FIXME: handle failed malloc
	gfs2_readi(sdp->md.statfs, buf, 0, sdp->md.statfs->i_di.di_size);
	/* call gfs2_inum_range_in() to retrieve range */
	gfs2_statfs_change_in(&sc, buf);
	free(buf);


	gfs2_lookupi(sdp->master_dir, "jindex", 6, &sdp->md.jiinode);

	gfs2_lookupi(sdp->master_dir, "rindex", 6, &sdp->md.riinode);

	gfs2_lookupi(sdp->master_dir, "quota", 5, &sdp->md.qinode);

	gfs2_lookupi(sdp->master_dir, "per_node", 8, &sdp->md.pinode);

	/* FIXME fill in per_node structure */

	/*******************************************************************
	 *******  Fill in rgrp and journal indexes and related fields  *****
	 *******************************************************************/

	/* read in the ji data */
	if (ji_update(sdp)){
		log_err( _("Unable to read in ji inode.\n"));
		return -1;
	}

	log_warn( _("Validating Resource Group index.\n"));
	for (trust_lvl = blind_faith; trust_lvl <= distrust; trust_lvl++) {
		log_warn( _("Level %d RG check.\n"), trust_lvl + 1);
		if ((rg_repair(sdp, trust_lvl, &rgcount) == 0) &&
		    (ri_update(sdp, 0, &rgcount) == 0)) {
			log_warn( _("(level %d passed)\n"), trust_lvl + 1);
			break;
		}
		else
			log_err( _("(level %d failed)\n"), trust_lvl + 1);
	}
	if (trust_lvl > distrust) {
		log_err( _("RG recovery impossible; I can't fix this file system.\n"));
		return -1;
	}
	log_info( _("%u resource groups found.\n"), rgcount);

	check_rgrps_integrity(sdp);

	/*******************************************************************
	 *******  Now, set boundary fields in the super block  *************
	 *******************************************************************/
	if(set_block_ranges(sdp)){
		log_err( _("Unable to determine the boundaries of the"
			" file system.\n"));
		goto fail;
	}

	bl = gfs2_bmap_create(sdp, last_fs_block+1, &addl_mem_needed);
	if (!bl) {
		log_crit( _("This system doesn't have enough memory + swap space to fsck this file system.\n"));
		log_crit( _("Additional memory needed is approximately: %lluMB\n"),
			 (unsigned long long)(addl_mem_needed / 1048576ULL));
		log_crit( _("Please increase your swap space by that amount and run gfs2_fsck again.\n"));
		goto fail;
	}
	osi_list_init(&dup_blocks.list);
	return 0;
 fail:
	empty_super_block(sdp);

	return -1;
}

/**
 * fill_super_block
 * @sdp:
 *
 * Returns: 0 on success, -1 on failure
 */
static int fill_super_block(struct gfs2_sbd *sdp)
{
	uint32_t i;

	sync();

	/********************************************************************
	 ***************** First, initialize all lists **********************
	 ********************************************************************/
	log_info( _("Initializing lists...\n"));
	osi_list_init(&sdp->rglist);
	for(i = 0; i < FSCK_HASH_SIZE; i++) {
		osi_list_init(&dir_hash[i]);
		osi_list_init(&inode_hash[i]);
	}

	/********************************************************************
	 ************  next, read in on-disk SB and set constants  **********
	 ********************************************************************/
	sdp->sd_sb.sb_bsize = GFS2_DEFAULT_BSIZE;
	sdp->bsize = sdp->sd_sb.sb_bsize;

	if(sizeof(struct gfs2_sb) > sdp->sd_sb.sb_bsize){
		log_crit( _("GFS superblock is larger than the blocksize!\n"));
		log_debug( _("sizeof(struct gfs2_sb) > sdp->sd_sb.sb_bsize\n"));
		return -1;
	}

	if (compute_constants(sdp)) {
		log_crit(_("Bad constants (1)\n"));
		exit(-1);
	}
	if(read_sb(sdp) < 0){
		return -1;
	}

	return 0;
}

/**
 * initialize - initialize superblock pointer
 *
 */
int initialize(struct gfs2_sbd *sbp, int force_check, int preen,
	       int *all_clean)
{
	int clean_journals = 0;

	*all_clean = 0;

	if(opts.no) {
		if ((sbp->device_fd = open(opts.device, O_RDONLY)) < 0) {
			log_crit( _("Unable to open device: %s\n"), opts.device);
			return FSCK_USAGE;
		}
	} else {
		/* read in sb from disk */
		if ((sbp->device_fd = open(opts.device, O_RDWR | O_EXCL)) < 0){
			if (errno == EBUSY)
				log_crit( _("Device %s is busy.\n"),
					 opts.device);
			else
				log_crit( _("Unable to open device: %s\n"),
					  opts.device);
			return FSCK_USAGE;
		}
	}
	if (fill_super_block(sbp)) {
		stack;
		return FSCK_ERROR;
	}

	/* Change lock protocol to be fsck_* instead of lock_* */
	if(!opts.no && preen_is_safe(sbp, preen, force_check)) {
		if(block_mounters(sbp, 1)) {
			log_err( _("Unable to block other mounters\n"));
			return FSCK_USAGE;
		}
	}

	/* verify various things */

	if(replay_journals(sbp, preen, force_check, &clean_journals)) {
		if(!opts.no && preen_is_safe(sbp, preen, force_check))
			block_mounters(sbp, 0);
		stack;
		return FSCK_ERROR;
	}
	if (sbp->md.journals == clean_journals)
		*all_clean = 1;
	else {
		if (force_check || !preen)
			log_notice( _("\nJournal recovery complete.\n"));
	}

	if (!force_check && *all_clean && preen)
		return FSCK_OK;

	if (init_system_inodes(sbp))
		return FSCK_ERROR;

	return FSCK_OK;
}

static void destroy_sbp(struct gfs2_sbd *sbp)
{
	if(!opts.no) {
		if(block_mounters(sbp, 0)) {
			log_warn( _("Unable to unblock other mounters - manual intervention required\n"));
			log_warn( _("Use 'gfs2_tool sb <device> proto' to fix\n"));
		}
		log_info( _("Syncing the device.\n"));
		fsync(sbp->device_fd);
	}
	empty_super_block(sbp);
	close(sbp->device_fd);
}

void destroy(struct gfs2_sbd *sbp)
{
	destroy_sbp(sbp);
}
