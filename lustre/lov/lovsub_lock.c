/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Implementation of cl_lock for LOVSUB layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 */

#define DEBUG_SUBSYSTEM S_LOV

#include "lov_cl_internal.h"

/** \addtogroup lov lov @{ */

/*****************************************************************************
 *
 * Lovsub lock operations.
 *
 */

static void lovsub_lock_fini(const struct lu_env *env,
                             struct cl_lock_slice *slice)
{
        struct lovsub_lock   *lsl;

        ENTRY;
        lsl = cl2lovsub_lock(slice);
        LASSERT(list_empty(&lsl->lss_parents));
        OBD_SLAB_FREE_PTR(lsl, lovsub_lock_kmem);
        EXIT;
}

static void lovsub_parent_lock(const struct lu_env *env, struct lov_lock *lov)
{
        struct cl_lock *parent;

        ENTRY;
        parent = lov->lls_cl.cls_lock;
        cl_lock_get(parent);
        lu_ref_add(&parent->cll_reference, "lovsub-parent", cfs_current());
        cl_lock_mutex_get(env, parent);
        EXIT;
}

static void lovsub_parent_unlock(const struct lu_env *env, struct lov_lock *lov)
{
        struct cl_lock *parent;

        ENTRY;
        parent = lov->lls_cl.cls_lock;
        cl_lock_mutex_put(env, lov->lls_cl.cls_lock);
        lu_ref_del(&parent->cll_reference, "lovsub-parent", cfs_current());
        cl_lock_put(env, parent);
        EXIT;
}

static int lovsub_lock_state_one(const struct lu_env *env,
                                 const struct lovsub_lock *lovsub,
                                 struct lov_lock *lov)
{
        struct cl_lock *parent;
        struct cl_lock *child;
        int             restart = 0;

        ENTRY;
        parent = lov->lls_cl.cls_lock;
        child  = lovsub->lss_cl.cls_lock;

        if (lovsub->lss_active != parent) {
                lovsub_parent_lock(env, lov);
                if (child->cll_error != 0 && parent->cll_error == 0) {
                        /*
                         * This is a deadlock case:
                         * cl_lock_error(for the parent lock)
                         *   -> cl_lock_delete
                         *     -> lov_lock_delete
                         *       -> cl_lock_enclosure
                         *         -> cl_lock_mutex_try(for the child lock)
                         */
                        cl_lock_mutex_put(env, child);
                        cl_lock_error(env, parent, child->cll_error);
                        restart = 1;
                } else {
                        cl_lock_signal(env, parent);
                }
                lovsub_parent_unlock(env, lov);
        }
        RETURN(restart);
}

/**
 * Implements cl_lock_operations::clo_state() method for lovsub layer, which
 * method is called whenever sub-lock state changes. Propagates state change
 * to the top-locks.
 */
static void lovsub_lock_state(const struct lu_env *env,
                              const struct cl_lock_slice *slice,
                              enum cl_lock_state state)
{
        struct lovsub_lock   *sub = cl2lovsub_lock(slice);
        struct lov_lock_link *scan;
        int                   restart = 0;

        LASSERT(cl_lock_is_mutexed(slice->cls_lock));
        ENTRY;

        do {
                restart = 0;
                list_for_each_entry(scan, &sub->lss_parents, lll_list) {
                        restart = lovsub_lock_state_one(env, sub,
                                                        scan->lll_super);
                        if (restart) {
                                cl_lock_mutex_get(env, slice->cls_lock);
                                break;
                        }
                }
        } while(restart);
        EXIT;
}

/**
 * Implementation of cl_lock_operation::clo_weigh() estimating lock weight by
 * asking parent lock.
 */
static unsigned long lovsub_lock_weigh(const struct lu_env *env,
                                       const struct cl_lock_slice *slice)
{
        struct lovsub_lock *lock = cl2lovsub_lock(slice);
        struct lov_lock    *lov;
        unsigned long       dumbbell;

        ENTRY;

        LASSERT(cl_lock_is_mutexed(slice->cls_lock));

        if (!list_empty(&lock->lss_parents)) {
                /*
                 * It is not clear whether all parents have to be asked and
                 * their estimations summed, or it is enough to ask one. For
                 * the current usages, one is always enough.
                 */
                lov = container_of(lock->lss_parents.next,
                                   struct lov_lock_link, lll_list)->lll_super;

                lovsub_parent_lock(env, lov);
                dumbbell = cl_lock_weigh(env, lov->lls_cl.cls_lock);
                lovsub_parent_unlock(env, lov);
        } else
                dumbbell = 0;

        RETURN(dumbbell);
}

/**
 * Maps start/end offsets within a stripe, to offsets within a file.
 */
static void lovsub_lock_descr_map(const struct cl_lock_descr *in,
                                  struct lov_object *obj,
                                  int stripe, struct cl_lock_descr *out)
{
        struct lov_stripe_md *lsm = lov_r0(obj)->lo_lsm;
        pgoff_t size; /* stripe size in pages */
        pgoff_t skip; /* how many pages in every stripe are occupied by
                       * "other" stripes */
        pgoff_t start;
        pgoff_t end;

        ENTRY;
        start = in->cld_start;
        end   = in->cld_end;

        /*
         * XXX join file support.
         */
        if (lsm->lsm_stripe_count > 1) {
                size = cl_index(lov2cl(obj), lsm->lsm_stripe_size);
                skip = (lsm->lsm_stripe_count - 1) * size;

                /* XXX overflow check here? */
                start += start/size * skip + stripe * size;

                if (end != CL_PAGE_EOF) {
                        end += end/size * skip + stripe * size;
                        /*
                         * And check for overflow...
                         */
                        if (end < in->cld_end)
                                end = CL_PAGE_EOF;
                }
        }
        out->cld_start = start;
        out->cld_end   = end;
        EXIT;
}

/**
 * Adjusts parent lock extent when a sub-lock is attached to a parent. This is
 * called in two ways:
 *
 *     - as part of receive call-back, when server returns granted extent to
 *       the client, and
 *
 *     - when top-lock finds existing sub-lock in the cache.
 *
 * Note, that lock mode is not propagated to the parent: i.e., if CLM_READ
 * top-lock matches CLM_WRITE sub-lock, top-lock is still CLM_READ.
 */
int lov_sublock_modify(const struct lu_env *env, struct lov_lock *lov,
                       struct lovsub_lock *sublock,
                       const struct cl_lock_descr *d, int idx)
{
        struct cl_lock       *parent;
        struct cl_lock       *child;
        struct lovsub_object *subobj;
        struct cl_lock_descr *pd;
        struct cl_lock_descr *parent_descr;
        int                   result;

        parent       = lov->lls_cl.cls_lock;
        parent_descr = &parent->cll_descr;
        LASSERT(cl_lock_mode_match(d->cld_mode, parent_descr->cld_mode));

        child  = sublock->lss_cl.cls_lock;
        subobj = cl2lovsub(sublock->lss_cl.cls_obj);
        pd     = &lov_env_info(env)->lti_ldescr;

        pd->cld_obj  = parent_descr->cld_obj;
        pd->cld_mode = parent_descr->cld_mode;
        lovsub_lock_descr_map(d, subobj->lso_super, subobj->lso_index, pd);
        lov->lls_sub[idx].sub_got = *d;
        /*
         * Notify top-lock about modification, if lock description changes
         * materially.
         */
        if (!cl_lock_ext_match(parent_descr, pd))
                result = cl_lock_modify(env, parent, pd);
        else
                result = 0;
        return result;
}

static int lovsub_lock_modify(const struct lu_env *env,
                              const struct cl_lock_slice *s,
                              const struct cl_lock_descr *d)
{
        struct lovsub_lock   *lock   = cl2lovsub_lock(s);
        struct lov_lock_link *scan;
        struct lov_lock      *lov;
        int result                   = 0;

        ENTRY;

        LASSERT(cl_lock_mode_match(d->cld_mode,
                                   s->cls_lock->cll_descr.cld_mode));
        list_for_each_entry(scan, &lock->lss_parents, lll_list) {
                int rc;

                lov = scan->lll_super;
                lovsub_parent_lock(env, lov);
                rc = lov_sublock_modify(env, lov, lock, d, scan->lll_idx);
                lovsub_parent_unlock(env, lov);
                result = result ?: rc;
        }
        RETURN(result);
}

static int lovsub_lock_closure(const struct lu_env *env,
                               const struct cl_lock_slice *slice,
                               struct cl_lock_closure *closure)
{
        struct lovsub_lock   *sub;
        struct cl_lock       *parent;
        struct lov_lock_link *scan;
        int                   result;

        LASSERT(cl_lock_is_mutexed(slice->cls_lock));
        ENTRY;

        sub    = cl2lovsub_lock(slice);
        result = 0;

        list_for_each_entry(scan, &sub->lss_parents, lll_list) {
                parent = scan->lll_super->lls_cl.cls_lock;
                result = cl_lock_closure_build(env, parent, closure);
                if (result != 0)
                        break;
        }
        RETURN(result);
}

/**
 * A helper function for lovsub_lock_delete() that deals with a given parent
 * top-lock.
 */
static int lovsub_lock_delete_one(const struct lu_env *env,
                                  struct cl_lock *child, struct lov_lock *lov)
{
        struct cl_lock       *parent;
        int             result;
        ENTRY;

        parent  = lov->lls_cl.cls_lock;
        result = 0;

        switch (parent->cll_state) {
        case CLS_NEW:
        case CLS_QUEUING:
        case CLS_ENQUEUED:
        case CLS_FREEING:
                cl_lock_signal(env, parent);
                break;
        case CLS_UNLOCKING:
                /*
                 * Here lies a problem: a sub-lock is canceled while top-lock
                 * is being unlocked. Top-lock cannot be moved into CLS_NEW
                 * state, because unlocking has to succeed eventually by
                 * placing lock into CLS_CACHED (or failing it), see
                 * cl_unuse_try(). Nor can top-lock be left in CLS_CACHED
                 * state, because lov maintains an invariant that all
                 * sub-locks exist in CLS_CACHED (this allows cached top-lock
                 * to be reused immediately). Nor can we wait for top-lock
                 * state to change, because this can be synchronous to the
                 * current thread.
                         *
                 * We know for sure that lov_lock_unuse() will be called at
                 * least one more time to finish un-using, so leave a mark on
                 * the top-lock, that will be seen by the next call to
                 * lov_lock_unuse().
                 */
                lov->lls_unuse_race = 1;
                break;
        case CLS_CACHED:
                /*
                 * if a sub-lock is canceled move its top-lock into CLS_NEW
                 * state to preserve an invariant that a top-lock in
                 * CLS_CACHED is immediately ready for re-use (i.e., has all
                 * sub-locks), and so that next attempt to re-use the top-lock
                 * enqueues missing sub-lock.
                 */
                cl_lock_state_set(env, parent, CLS_NEW);
                /*
                 * if last sub-lock is canceled, destroy the top-lock (which
                 * is now `empty') proactively.
                 */
                if (lov->lls_nr_filled == 0) {
                        /* ... but unfortunately, this cannot be done easily,
                         * as cancellation of a top-lock might acquire mutices
                         * of its other sub-locks, violating lock ordering,
                         * see cl_lock_{cancel,delete}() preconditions.
                         *
                         * To work around this, the mutex of this sub-lock is
                         * released, top-lock is destroyed, and sub-lock mutex
                         * acquired again. The list of parents has to be
                         * re-scanned from the beginning after this.
                         *
                         * Only do this if no mutices other than on @child and
                         * @parent are held by the current thread.
                         *
                         * TODO: The lock modal here is too complex, because
                         * the lock may be canceled and deleted by voluntarily:
                         *    cl_lock_request
                         *      -> osc_lock_enqueue_wait
                         *        -> osc_lock_cancel_wait
                         *          -> cl_lock_delete
                         *            -> lovsub_lock_delete
                         *              -> cl_lock_cancel/delete
                         *                -> ...
                         *
                         * The better choice is to spawn a kernel thread for
                         * this purpose. -jay
                         */
                        if (cl_lock_nr_mutexed(env) == 2) {
                                cl_lock_mutex_put(env, child);
                                cl_lock_cancel(env, parent);
                                cl_lock_delete(env, parent);
                                result = 1;
                        }
                }
                break;
        case CLS_HELD:
        default:
                CERROR("Impossible state: %i\n", parent->cll_state);
                LBUG();
        }

        RETURN(result);
}

/**
 * An implementation of cl_lock_operations::clo_delete() method. This is
 * invoked in "bottom-to-top" delete, when lock destruction starts from the
 * sub-lock (e.g, as a result of ldlm lock LRU policy).
 */
static void lovsub_lock_delete(const struct lu_env *env,
                               const struct cl_lock_slice *slice)
{
        struct cl_lock     *child = slice->cls_lock;
        struct lovsub_lock *sub   = cl2lovsub_lock(slice);
        int restart;

        LASSERT(cl_lock_is_mutexed(child));

        ENTRY;
        /*
         * Destruction of a sub-lock might take multiple iterations, because
         * when the last sub-lock of a given top-lock is deleted, top-lock is
         * canceled proactively, and this requires to release sub-lock
         * mutex. Once sub-lock mutex has been released, list of its parents
         * has to be re-scanned from the beginning.
         */
        do {
                struct lov_lock      *lov;
                struct lov_lock_link *scan;
                struct lov_lock_link *temp;
                struct lov_lock_sub  *subdata;

                restart = 0;
                list_for_each_entry_safe(scan, temp,
                                         &sub->lss_parents, lll_list) {
                        lov     = scan->lll_super;
                        subdata = &lov->lls_sub[scan->lll_idx];
                        lovsub_parent_lock(env, lov);
                        subdata->sub_got = subdata->sub_descr;
                        lov_lock_unlink(env, scan, sub);
                        restart = lovsub_lock_delete_one(env, child, lov);
                        lovsub_parent_unlock(env, lov);

                        if (restart) {
                                cl_lock_mutex_get(env, child);
                                break;
                        }
               }
        } while (restart);
        EXIT;
}

static int lovsub_lock_print(const struct lu_env *env, void *cookie,
                             lu_printer_t p, const struct cl_lock_slice *slice)
{
        struct lovsub_lock   *sub = cl2lovsub_lock(slice);
        struct lov_lock      *lov;
        struct lov_lock_link *scan;

        list_for_each_entry(scan, &sub->lss_parents, lll_list) {
                lov = scan->lll_super;
                (*p)(env, cookie, "[%d %p ", scan->lll_idx, lov);
                if (lov != NULL)
                        cl_lock_descr_print(env, cookie, p,
                                            &lov->lls_cl.cls_lock->cll_descr);
                (*p)(env, cookie, "] ");
        }
        return 0;
}

static const struct cl_lock_operations lovsub_lock_ops = {
        .clo_fini    = lovsub_lock_fini,
        .clo_state   = lovsub_lock_state,
        .clo_delete  = lovsub_lock_delete,
        .clo_modify  = lovsub_lock_modify,
        .clo_closure = lovsub_lock_closure,
        .clo_weigh   = lovsub_lock_weigh,
        .clo_print   = lovsub_lock_print
};

int lovsub_lock_init(const struct lu_env *env, struct cl_object *obj,
                     struct cl_lock *lock, const struct cl_io *io)
{
        struct lovsub_lock *lsk;
        int result;

        ENTRY;
        OBD_SLAB_ALLOC_PTR_GFP(lsk, lovsub_lock_kmem, CFS_ALLOC_IO);
        if (lsk != NULL) {
                CFS_INIT_LIST_HEAD(&lsk->lss_parents);
                cl_lock_slice_add(lock, &lsk->lss_cl, obj, &lovsub_lock_ops);
                result = 0;
        } else
                result = -ENOMEM;
        RETURN(result);
}

/** @} lov */
