/*
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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ldlm/ldlm_extent.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 */

/**
 * This file contains implementation of EXTENT lock type
 *
 * EXTENT lock type is for locking a contiguous range of values, represented
 * by 64-bit starting and ending offsets (inclusive). There are several extent
 * lock modes, some of which may be mutually incompatible. Extent locks are
 * considered incompatible if their modes are incompatible and their extents
 * intersect.  See the lock mode compatibility matrix in lustre_dlm.h.
 */

#define DEBUG_SUBSYSTEM S_LDLM
#include <linux/libcfs/libcfs.h>
#include <lustre_dlm.h>
#include <obd_support.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_lib.h>
#include "ldlm_internal.h"

/* When a lock is cancelled by a client, the KMS may undergo change if this
 * is the "highest lock".  This function returns the new KMS value.
 * Caller must hold lr_lock already.
 *
 * NB: A lock on [x,y] protects a KMS of up to y + 1 bytes!
 */
__u64 ldlm_extent_shift_kms(struct ldlm_lock *lock, __u64 old_kms)
{
	struct ldlm_resource *res = lock->l_resource;
	struct list_head *tmp;
	struct ldlm_lock *lck;
	__u64 kms = 0;

	/* don't let another thread in ldlm_extent_shift_kms race in
	 * just after we finish and take our lock into account in its
	 * calculation of the kms
	 */
	ldlm_set_kms_ignore(lock);

	list_for_each(tmp, &res->lr_granted) {
		lck = list_entry(tmp, struct ldlm_lock, l_res_link);

		if (ldlm_is_kms_ignore(lck))
			continue;

		if (lck->l_policy_data.l_extent.end >= old_kms)
			return old_kms;

		/* This extent _has_ to be smaller than old_kms (checked above)
		 * so kms can only ever be smaller or the same as old_kms.
		 */
		if (lck->l_policy_data.l_extent.end + 1 > kms)
			kms = lck->l_policy_data.l_extent.end + 1;
	}
	LASSERTF(kms <= old_kms, "kms %llu old_kms %llu\n", kms, old_kms);

	return kms;
}
EXPORT_SYMBOL(ldlm_extent_shift_kms);

struct kmem_cache *ldlm_interval_slab;

/* interval tree, for LDLM_EXTENT. */
static void ldlm_interval_attach(struct ldlm_interval *n, struct ldlm_lock *l)
{
	LASSERT(!l->l_tree_node);
	LASSERT(l->l_resource->lr_type == LDLM_EXTENT);

	list_add_tail(&l->l_sl_policy, &n->li_group);
	l->l_tree_node = n;
}

struct ldlm_interval *ldlm_interval_alloc(struct ldlm_lock *lock)
{
	struct ldlm_interval *node;

	LASSERT(lock->l_resource->lr_type == LDLM_EXTENT);
	node = kmem_cache_zalloc(ldlm_interval_slab, GFP_NOFS);
	if (!node)
		return NULL;

	INIT_LIST_HEAD(&node->li_group);
	ldlm_interval_attach(node, lock);
	return node;
}

void ldlm_interval_free(struct ldlm_interval *node)
{
	if (node) {
		LASSERT(list_empty(&node->li_group));
		LASSERT(!interval_is_intree(&node->li_node));
		kmem_cache_free(ldlm_interval_slab, node);
	}
}

struct ldlm_interval *ldlm_interval_detach(struct ldlm_lock *l)
{
	struct ldlm_interval *n = l->l_tree_node;

	if (!n)
		return NULL;

	LASSERT(!list_empty(&n->li_group));
	l->l_tree_node = NULL;
	list_del_init(&l->l_sl_policy);

	return list_empty(&n->li_group) ? n : NULL;
}

static inline int lock_mode_to_index(enum ldlm_mode mode)
{
	int index;

	LASSERT(mode != 0);
	LASSERT(is_power_of_2(mode));
	for (index = -1; mode; index++)
		mode >>= 1;
	LASSERT(index < LCK_MODE_NUM);
	return index;
}

/** Add newly granted lock into interval tree for the resource. */
void ldlm_extent_add_lock(struct ldlm_resource *res,
			  struct ldlm_lock *lock)
{
	struct interval_node *found, **root;
	struct ldlm_interval *node;
	struct ldlm_extent *extent;
	int idx, rc;

	LASSERT(lock->l_granted_mode == lock->l_req_mode);

	node = lock->l_tree_node;
	LASSERT(node);
	LASSERT(!interval_is_intree(&node->li_node));

	idx = lock_mode_to_index(lock->l_granted_mode);
	LASSERT(lock->l_granted_mode == 1 << idx);
	LASSERT(lock->l_granted_mode == res->lr_itree[idx].lit_mode);

	/* node extent initialize */
	extent = &lock->l_policy_data.l_extent;
	rc = interval_set(&node->li_node, extent->start, extent->end);
	LASSERT(!rc);

	root = &res->lr_itree[idx].lit_root;
	found = interval_insert(&node->li_node, root);
	if (found) { /* The policy group found. */
		struct ldlm_interval *tmp;

		tmp = ldlm_interval_detach(lock);
		ldlm_interval_free(tmp);
		ldlm_interval_attach(to_ldlm_interval(found), lock);
	}
	res->lr_itree[idx].lit_size++;

	/* even though we use interval tree to manage the extent lock, we also
	 * add the locks into grant list, for debug purpose, ..
	 */
	ldlm_resource_add_lock(res, &res->lr_granted, lock);

	if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_GRANT_CHECK)) {
		struct ldlm_lock *lck;

		list_for_each_entry_reverse(lck, &res->lr_granted,
					    l_res_link) {
			if (lck == lock)
				continue;
			if (lockmode_compat(lck->l_granted_mode,
					    lock->l_granted_mode))
				continue;
			if (ldlm_extent_overlap(&lck->l_req_extent,
						&lock->l_req_extent)) {
				CDEBUG(D_ERROR,
				       "granting conflicting lock %p %p\n",
				       lck, lock);
				ldlm_resource_dump(D_ERROR, res);
				LBUG();
			}
		}
	}
}

/** Remove cancelled lock from resource interval tree. */
void ldlm_extent_unlink_lock(struct ldlm_lock *lock)
{
	struct ldlm_resource *res = lock->l_resource;
	struct ldlm_interval *node = lock->l_tree_node;
	struct ldlm_interval_tree *tree;
	int idx;

	if (!node || !interval_is_intree(&node->li_node)) /* duplicate unlink */
		return;

	idx = lock_mode_to_index(lock->l_granted_mode);
	LASSERT(lock->l_granted_mode == 1 << idx);
	tree = &res->lr_itree[idx];

	LASSERT(tree->lit_root); /* assure the tree is not null */

	tree->lit_size--;
	node = ldlm_interval_detach(lock);
	if (node) {
		interval_erase(&node->li_node, &tree->lit_root);
		ldlm_interval_free(node);
	}
}

void ldlm_extent_policy_wire_to_local(const union ldlm_wire_policy_data *wpolicy,
				      union ldlm_policy_data *lpolicy)
{
	lpolicy->l_extent.start = wpolicy->l_extent.start;
	lpolicy->l_extent.end = wpolicy->l_extent.end;
	lpolicy->l_extent.gid = wpolicy->l_extent.gid;
}

void ldlm_extent_policy_local_to_wire(const union ldlm_policy_data *lpolicy,
				      union ldlm_wire_policy_data *wpolicy)
{
	memset(wpolicy, 0, sizeof(*wpolicy));
	wpolicy->l_extent.start = lpolicy->l_extent.start;
	wpolicy->l_extent.end = lpolicy->l_extent.end;
	wpolicy->l_extent.gid = lpolicy->l_extent.gid;
}
