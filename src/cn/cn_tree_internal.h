/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_KVDB_CN_CN_TREE_INTERNAL_H
#define HSE_KVDB_CN_CN_TREE_INTERNAL_H

/* MTF_MOCK_DECL(cn_tree_internal) */

#include <hse_util/mutex.h>
#include <hse_util/list.h>

#include <hse/hse_limits.h>

#include <hse_ikvdb/sched_sts.h>

#include "cn_tree.h"
#include "cn_tree_iter.h"
#include "cn_metrics.h"
#include "omf.h"

#include "csched_sp3.h"

struct hlog;

/* Each node in a cN tree contains a list of kvsets that must be protected
 * against concurrent update.  Since update of the list is relatively rare,
 * we optimize the read path to avoid contention on what would otherwise be
 * a per-list lock.  To protect a kvset list for read-only access, a thread
 * must acquire a read lock on any one of the locks in the vector of locks
 * in the cN tree (i.e., tree->ct_bktv[]).  To update/modify a kvset list,
 * a thread must acquire a write lock on each and every lock in ct_bktv[].
 */

#define RMLOCK_MAX (128)

/**
 * A "read-mostly" lock.
 *
 * [HSE_REVISIT] Move this into platform and formalize the API.  Maybe
 * replace with prwlock...
 */
struct rmlock_bkt {
    u64                 rm_rwcnt;
    struct rw_semaphore rm_lock;
} __aligned(SMP_CACHE_BYTES);

struct rmlock {
    atomic_t rm_writer;
    u32      rm_bktmax;

    struct rmlock_bkt rm_bktv[RMLOCK_MAX + 1];
};

#define rmlock_cmpxchg(_ptr, _oldp, _new) \
    __atomic_compare_exchange_n((_ptr), (_oldp), (_new), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/**
 * struct cn_kle_cache - kvset list entry cache
 * @kc_lock:    protects %ic_npages and %kc_pages
 * @kc_npages:  number of pages in cache
 * @kc_pages:   list of pages in cache
 *
 * The kvset list entry cache keeps the kvset list entry
 * nodes co-located to minimize pages faults during
 * cn tree traversals.  Each page in the cache contains
 * a header (cn_kle_hdr) followed by as many kvset list
 * entry objects as will fit into the page.
 */
struct cn_kle_cache {
    spinlock_t       kc_lock;
    int              kc_npages;
    struct list_head kc_pages;
};

struct cn_kle_hdr {
    struct list_head kh_link;
    struct list_head kh_entries;
    ulong            kh_nallocs;
    ulong            kh_nfrees;
} __aligned(SMP_CACHE_BYTES);

#define CN_KHASHMAP_SHIFT (8)

/**
 * struct cn_khashmap - key hash map
 * @khm_mapv:
 * @khm_gen_committed:
 * @khm_gen:
 * @khm_lock:
 */
struct cn_khashmap {
    spinlock_t khm_lock;
    u32        khm_gen;
    u32        khm_gen_committed;
    u8         khm_mapv[CN_TSTATE_KHM_SZ];
};

/**
 * struct cn_tree - the cn tree (tree of nodes holding kvsets)
 * @ct_root:        root node of tree
 * @ct_khashmap:    ptr to key hash map
 * @ct_cp:          cn create-time parameters
 * @ct_fanout_bits: log base2 of tree fanout
 * @ds:    dataset
 * @cn:    ptr to parent cn object
 * @rp:    ptr to shared runtime parameters struct
 * @cndb:  handle for cndb (the metadata journal/log)
 * @cnid:  cndb's identifier for this cn tree
 * @ct_fanout_mask: fanout bit mask (@ct_fanout - 1)
 * @ct_depth_max:   depth limit for this tree (not current depth)
 * @ct_dgen_init:
 * @ct_r_nodec:
 * @ct_l_nodec:
 * @ct_l_samp:
 * @ct_sched:
 * @ct_kvdb_health: for monitoring KDVB health
 * @ct_nospace:     set when "disk is full"
 * @ct_iter:        iterate over tree nodes for compaction
 * @ct_last_ptseq:
 * @ct_last_ptlen:  length of @ct_last_ptomb
 * @ct_last_ptomb:  if cn is a capped, this holds the last (largest) ptomb in cn
 * @ct_kle_cache:   kvset list entry cache
 * @ct_lock:        read-mostly lock to protect kvset list
 *
 * Note: The first fields are frequently accessed in the order listed
 * (e.g., by cn_tree_lookup) and are read-only after initialization.
 */
struct cn_tree {
    struct cn_tree_node *ct_root;
    struct cn_khashmap * ct_khashmap;
    u16                  ct_fanout_bits;
    u16                  ct_pfx_len;
    uint                 ct_fanout_mask;
    u16                  ct_depth_max;
    u16                  ct_sfx_len;
    bool                 ct_nospace;
    struct cn *          cn;
    struct mpool *       ds;
    struct kvs_rparams * rp;

    struct cn_khashmap ct_khmbuf;
    struct cn_tstate * ct_tstate;

    struct cndb *       cndb;
    struct cn_kvdb *    cn_kvdb;
    struct kvs_cparams *ct_cp;
    u64                 cnid;
    u64                 ct_dgen_init;

    uint                 ct_i_nodec;
    uint                 ct_l_nodec;
    uint                 ct_lvl_max;
    struct cn_samp_stats ct_samp;

    __aligned(SMP_CACHE_BYTES) union {
        struct sp3_tree sp3t;
    } ct_sched;

    u64                      ct_capped_ttl;
    u64                      ct_capped_dgen;
    struct kvset_list_entry *ct_capped_le;

    __aligned(SMP_CACHE_BYTES) struct kvdb_health *ct_kvdb_health;

    u64 ct_last_ptseq;
    u32 ct_last_ptlen;
    u8  ct_last_ptomb[HSE_KVS_MAX_PFXLEN];

    __aligned(SMP_CACHE_BYTES) struct cn_kle_cache ct_kle_cache;

    __aligned(SMP_CACHE_BYTES) struct rmlock ct_lock;
};

/**
 * struct cn_tree_node - A node in a k-way cn_tree
 * @tn_rspills_lock:  lock to protect @tn_rspills
 * @tn_rspills:       list of active spills from this node to its children
 * @tn_compacting:   true if node is being compacted
 * @tn_hlog:         hyperloglog structure
 * @tn_add_cntr:
 * @tn_rem_cntr:
 * @tn_stats_add_cntr:
 * @tn_stats_rem_cntr:
 * @tn_ns:           metrics about node to guide node compaction decisions
 * @tn_loc:          location of node within tree
 * @tn_kvset_cnt:    number of kvsets  in node
 * @tn_pfx_spill:    true if spills/scans from this node use the prefix hash
 * @tn_tree:         ptr to tree struct
 * @tn_parent:       parent node
 * @tn_child:        child nodes
 */
struct cn_tree_node {
    struct mutex     tn_rspills_lock;
    struct list_head tn_rspills;
    u64              tn_biggest_kvset; /* key count */
    bool             tn_rspills_wedged;
    u8               tn_childc;
    atomic_t         tn_compacting;

    union {
        struct sp3_node sp3n;
    } tn_sched;

    __aligned(SMP_CACHE_BYTES) struct hlog *tn_hlog;
    struct cn_node_stats tn_ns;
    struct cn_samp_stats tn_samp;
    u64                  tn_size_max;
    u64                  tn_update_incr_dgen;

    __aligned(SMP_CACHE_BYTES) struct cn_node_loc tn_loc;
    bool                 tn_terminal_node_warning;
    bool                 tn_pfx_spill;
    struct list_head     tn_kvset_list; /* head = newest kvset */
    struct cn_tree *     tn_tree;
    struct cn_tree_node *tn_parent;
    struct cn_tree_node *tn_childv[];
};

/* cn_tree_node to sp3_node */
#define tn2spn(_tn) (&(_tn)->tn_sched.sp3n)
#define spn2tn(_spn) container_of(_spn, struct cn_tree_node, tn_sched.sp3n)

/**
 * cn_tree_find_node - map a node location to a node pointer
 * @tree: cn tree to look for node
 * @loc:  location to map into a node pointer
 */
/* MTF_MOCK */
struct cn_tree_node *
cn_tree_find_node(struct cn_tree *tree, struct cn_node_loc *loc);

/* MTF_MOCK */
merr_t
cn_tree_create_node(
    struct cn_tree *      handle,
    uint                  node_level,
    uint                  node_offset,
    struct cn_tree_node **node);

void
rmlock_rlock(struct rmlock *lock, void **cookiep);

void
rmlock_runlock(void *cookie);

/* MTF_MOCK */
void
cn_node_stats_get(const struct cn_tree_node *tn, struct cn_node_stats *stats);

/* MTF_MOCK */
bool
cn_node_isleaf(const struct cn_tree_node *node);

bool
cn_node_isroot(const struct cn_tree_node *node);

/* MTF_MOCK */
uint
cn_node_level(const struct cn_tree_node *node);

/* MTF_MOCK */
void
cn_comp(struct cn_compaction_work *w);

/* MTF_MOCK */
void
cn_comp_cancel_cb(struct sts_job *job);

/* MTF_MOCK */
void
cn_comp_slice_cb(struct sts_job *job);

#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
#include "cn_tree_internal_ut.h"
#endif /* HSE_UNIT_TEST_MODE */

#endif /* HSE_KVDB_CN_CN_TREE_INTERNAL_H */
