/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mds/mds_orphan.c
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* code for handling open unlinked files */

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>

#include <portals/list.h>
#include <linux/obd_class.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_commit_confd.h>
#include <linux/lvfs.h>

#include "mds_internal.h"

static int mds_osc_destroy_orphan(struct mds_obd *mds,
                                  struct inode *inode,
                                  struct lov_mds_md *lmm,
                                  int lmm_size,
                                  struct llog_cookie *logcookies,
                                  int log_unlink)
{
        struct lov_stripe_md *lsm = NULL;
        struct obd_trans_info oti = { 0 };
        struct obdo *oa;
        int rc;
        ENTRY;

        if (lmm_size == 0)
                RETURN(0);

        rc = obd_unpackmd(mds->mds_osc_exp, &lsm, lmm, lmm_size);
        if (rc < 0) {
                CERROR("Error unpack md %p\n", lmm);
                RETURN(rc);
        } else {
                LASSERT(rc >= sizeof(*lsm));
                rc = 0;
        }

        oa = obdo_alloc();
        if (oa == NULL)
                GOTO(out_free_memmd, rc = -ENOMEM);
        oa->o_id = lsm->lsm_object_id;
        oa->o_mode = inode->i_mode & S_IFMT;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE;

        if (log_unlink && logcookies) {
                oa->o_valid |= OBD_MD_FLCOOKIE;
                oti.oti_logcookies = logcookies;
        }

        rc = obd_destroy(mds->mds_osc_exp, oa, lsm, &oti);
        obdo_free(oa);
        if (rc)
                CDEBUG(D_INODE, "destroy orphan objid 0x"LPX64" on ost error "
                       "%d\n", lsm->lsm_object_id, rc);
out_free_memmd:
        obd_free_memmd(mds->mds_osc_exp, &lsm);
        RETURN(rc);
}

static int mds_unlink_orphan(struct obd_device *obd, struct dentry *dchild,
                             struct inode *inode, struct inode *pending_dir)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lov_mds_md *lmm = NULL;
        struct llog_cookie *logcookies = NULL;
        int lmm_size, log_unlink = 0;
        void *handle = NULL;
        int rc, err;
        ENTRY;

        LASSERT(mds->mds_osc_obd != NULL);

        /* We don't need to do any of these other things for orhpan dirs,
         * especially not mds_get_md (may get a default LOV EA, bug 4554) */
        if (S_ISDIR(inode->i_mode)) {
                rc = vfs_rmdir(pending_dir, dchild);
                if (rc)
                        CERROR("error %d unlinking dir %*s from PENDING\n",
                               rc, dchild->d_name.len, dchild->d_name.name);
                RETURN(rc);
        }

        lmm_size = mds->mds_max_mdsize;
        OBD_ALLOC(lmm, lmm_size);
        if (lmm == NULL)
                RETURN(-ENOMEM);

        rc = mds_get_md(obd, inode, lmm, &lmm_size, 1);
        if (rc < 0)
                GOTO(out_free_lmm, rc);

        handle = fsfilt_start_log(obd, pending_dir, FSFILT_OP_UNLINK, NULL,
                                  le32_to_cpu(lmm->lmm_stripe_count));
        if (IS_ERR(handle)) {
                rc = PTR_ERR(handle);
                CERROR("error fsfilt_start: %d\n", rc);
                handle = NULL;
                GOTO(out_free_lmm, rc);
        }

        rc = vfs_unlink(pending_dir, dchild);
        if (rc) {
                CERROR("error %d unlinking orphan %.*s from PENDING\n",
                       rc, dchild->d_name.len, dchild->d_name.name);
        } else if (lmm_size) {
                OBD_ALLOC(logcookies, mds->mds_max_cookiesize);
                if (logcookies == NULL)
                        rc = -ENOMEM;
                else if (mds_log_op_unlink(obd, inode, lmm,lmm_size,logcookies,
                                           mds->mds_max_cookiesize) > 0)
                        log_unlink = 1;
        }

        err = fsfilt_commit(obd, pending_dir, handle, 0);
        if (err) {
                CERROR("error committing orphan unlink: %d\n", err);
                if (!rc)
                        rc = err;
        } else if (!rc) {
                rc = mds_osc_destroy_orphan(mds, inode, lmm, lmm_size,
                                            logcookies, log_unlink);
        }

        if (logcookies != NULL)
                OBD_FREE(logcookies, mds->mds_max_cookiesize);
out_free_lmm:
        OBD_FREE(lmm, mds->mds_max_mdsize);
        RETURN(rc);
}

int mds_cleanup_orphans(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lvfs_run_ctxt saved;
        struct file *file;
        struct dentry *dchild, *dentry;
        struct vfsmount *mnt;
        struct inode *child_inode, *pending_dir = mds->mds_pending_dir->d_inode;
        struct l_linux_dirent *dirent, *n;
        struct list_head dentry_list;
        char d_name[LL_FID_NAMELEN];
        unsigned long inum;
        int i = 0, rc = 0, item = 0, namlen;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = dget(mds->mds_pending_dir);
        if (IS_ERR(dentry))
                GOTO(err_pop, rc = PTR_ERR(dentry));
        mnt = mntget(mds->mds_vfsmnt);
        if (IS_ERR(mnt))
                GOTO(err_mntget, rc = PTR_ERR(mnt));

        file = dentry_open(mds->mds_pending_dir, mds->mds_vfsmnt,
                           O_RDONLY | O_LARGEFILE);
        if (IS_ERR(file))
                GOTO(err_pop, rc = PTR_ERR(file));

        INIT_LIST_HEAD(&dentry_list);
        rc = l_readdir(file, &dentry_list);
        filp_close(file, 0);
        if (rc < 0)
                GOTO(err_out, rc);

        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                i++;
                list_del(&dirent->lld_list);

                namlen = strlen(dirent->lld_name);
                LASSERT(sizeof(d_name) >= namlen + 1);
                strcpy(d_name, dirent->lld_name);
                inum = dirent->lld_ino;
                OBD_FREE(dirent, sizeof(*dirent));

                CDEBUG(D_INODE, "entry %d of PENDING DIR: %s\n", i, d_name);

                if (((namlen == 1) && !strcmp(d_name, ".")) ||
                    ((namlen == 2) && !strcmp(d_name, "..")) || inum == 0)
                        continue;

                down(&pending_dir->i_sem);
                dchild = lookup_one_len(d_name, mds->mds_pending_dir, namlen);
                if (IS_ERR(dchild)) {
                        up(&pending_dir->i_sem);
                        GOTO(err_out, rc = PTR_ERR(dchild));
                }
                if (!dchild->d_inode) {
                        CERROR("orphan %s has been removed\n", d_name);
                        GOTO(next, rc = 0);
                }

                if (is_bad_inode(dchild->d_inode)) {
                        CERROR("bad orphan inode found %lu/%u\n",
                               dchild->d_inode->i_ino,
                               dchild->d_inode->i_generation);
                        GOTO(next, rc = -ENOENT);
                }

                child_inode = dchild->d_inode;
                DOWN_READ_I_ALLOC_SEM(child_inode);
                if (mds_inode_is_orphan(child_inode) &&
                    mds_orphan_open_count(child_inode)) {
                        UP_READ_I_ALLOC_SEM(child_inode);
                        CWARN("orphan %s re-opened during recovery\n", d_name);
                        GOTO(next, rc = 0);
                }
                UP_READ_I_ALLOC_SEM(child_inode);

                rc = mds_unlink_orphan(obd, dchild, child_inode, pending_dir);
                if (rc == 0) {
                        item ++;
                        CWARN("removed orphan %s from MDS and OST\n", d_name);
                } else {
                        CDEBUG(D_INODE, "removed orphan %s from MDS/OST failed,"
                               " rc = %d\n", d_name, rc);
                        rc = 0;
                }
next:
                l_dput(dchild);
                up(&pending_dir->i_sem);
        }
err_out:
        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                list_del(&dirent->lld_list);
                OBD_FREE(dirent, sizeof(*dirent));
        }
err_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc == 0)
                rc = item;
        RETURN(rc);

err_mntget:
        l_dput(mds->mds_pending_dir);
        goto err_pop;
}
