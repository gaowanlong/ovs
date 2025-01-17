/*
 * Copyright (c) 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <errno.h>
#include <stdbool.h>

#include "bitmap.h"
#include "compiler.h"
#include "hmap.h"
#include "match.h"
#include "nx-match.h"
#include "odp-netlink.h"
#include "ofp-util.h"
#include "ovs-thread.h"
#include "ovs-rcu.h"
#include "packets.h"
#include "tun-metadata.h"

struct tun_meta_entry {
    struct hmap_node node;      /* In struct tun_table's key_hmap. */
    uint32_t key;               /* (class << 16) | type. */
    struct tun_metadata_loc loc;
    bool valid;                 /* True if allocated to a class and type. */
};

/* Maps from TLV option class+type to positions in a struct tun_metadata's
 * 'opts' array.  */
struct tun_table {
    /* TUN_METADATA<i> is stored in element <i>. */
    struct tun_meta_entry entries[TUN_METADATA_NUM_OPTS];

    /* Each bit represents 4 bytes of space, 0-bits are free space. */
    unsigned long alloc_map[BITMAP_N_LONGS(TUN_METADATA_TOT_OPT_SIZE / 4)];

    /* The valid elements in entries[], indexed by class+type. */
    struct hmap key_hmap;
};
BUILD_ASSERT_DECL(TUN_METADATA_TOT_OPT_SIZE % 4 == 0);

static struct ovs_mutex tab_mutex = OVS_MUTEX_INITIALIZER;
static OVSRCU_TYPE(struct tun_table *) metadata_tab;

static enum ofperr tun_metadata_add_entry(struct tun_table *map, uint8_t idx,
                                          uint16_t opt_class, uint8_t type,
                                          uint8_t len) OVS_REQUIRES(tab_mutex);
static void tun_metadata_del_entry(struct tun_table *map, uint8_t idx)
            OVS_REQUIRES(tab_mutex);
static void memcpy_to_metadata(struct tun_metadata *dst, const void *src,
                               const struct tun_metadata_loc *,
                               unsigned int idx);
static void memcpy_from_metadata(void *dst, const struct tun_metadata *src,
                                 const struct tun_metadata_loc *);

static uint32_t
tun_meta_key(ovs_be16 class, uint8_t type)
{
    return (OVS_FORCE uint16_t)class << 8 | type;
}

static ovs_be16
tun_key_class(uint32_t key)
{
    return (OVS_FORCE ovs_be16)(key >> 8);
}

static uint8_t
tun_key_type(uint32_t key)
{
    return key & 0xff;
}

/* Returns a newly allocated tun_table.  If 'old_map' is nonnull then the new
 * tun_table is a deep copy of the old one. */
static struct tun_table *
table_alloc(const struct tun_table *old_map) OVS_REQUIRES(tab_mutex)
{
    struct tun_table *new_map;

    new_map = xzalloc(sizeof *new_map);

    if (old_map) {
        struct tun_meta_entry *entry;

        *new_map = *old_map;
        hmap_init(&new_map->key_hmap);

        HMAP_FOR_EACH (entry, node, &old_map->key_hmap) {
            struct tun_meta_entry *new_entry;
            struct tun_metadata_loc_chain *chain;

            new_entry = &new_map->entries[entry - old_map->entries];
            hmap_insert(&new_map->key_hmap, &new_entry->node, entry->node.hash);

            chain = &new_entry->loc.c;
            while (chain->next) {
                chain->next = xmemdup(chain->next, sizeof *chain->next);
                chain = chain->next;
            }
        }
    } else {
        hmap_init(&new_map->key_hmap);
    }

    return new_map;
}

/* Frees 'map' and all the memory it owns. */
static void
table_free(struct tun_table *map) OVS_REQUIRES(tab_mutex)
{
    struct tun_meta_entry *entry;

    if (!map) {
        return;
    }

    HMAP_FOR_EACH (entry, node, &map->key_hmap) {
        tun_metadata_del_entry(map, entry - map->entries);
    }

    free(map);
}

/* Creates a global tunnel metadata mapping table, if none already exists. */
void
tun_metadata_init(void)
{
    ovs_mutex_lock(&tab_mutex);

    if (!ovsrcu_get_protected(struct tun_table *, &metadata_tab)) {
        ovsrcu_set(&metadata_tab, table_alloc(NULL));
    }

    ovs_mutex_unlock(&tab_mutex);
}

enum ofperr
tun_metadata_table_mod(struct ofputil_tlv_table_mod *ttm)
{
    struct tun_table *old_map, *new_map;
    struct ofputil_tlv_map *ofp_map;
    enum ofperr err = 0;

    ovs_mutex_lock(&tab_mutex);

    old_map = ovsrcu_get_protected(struct tun_table *, &metadata_tab);

    switch (ttm->command) {
    case NXTTMC_ADD:
        new_map = table_alloc(old_map);

        LIST_FOR_EACH (ofp_map, list_node, &ttm->mappings) {
            err = tun_metadata_add_entry(new_map, ofp_map->index,
                                         ofp_map->option_class,
                                         ofp_map->option_type,
                                         ofp_map->option_len);
            if (err) {
                table_free(new_map);
                goto out;
            }
        }
        break;

    case NXTTMC_DELETE:
        new_map = table_alloc(old_map);

        LIST_FOR_EACH (ofp_map, list_node, &ttm->mappings) {
            tun_metadata_del_entry(new_map, ofp_map->index);
        }
        break;

    case NXTTMC_CLEAR:
        new_map = table_alloc(NULL);
        break;

    default:
        OVS_NOT_REACHED();
    }

    ovsrcu_set(&metadata_tab, new_map);
    ovsrcu_postpone(table_free, old_map);

out:
    ovs_mutex_unlock(&tab_mutex);
    return err;
}

void
tun_metadata_table_request(struct ofputil_tlv_table_reply *ttr)
{
    struct tun_table *map = ovsrcu_get(struct tun_table *, &metadata_tab);
    int i;

    ttr->max_option_space = TUN_METADATA_TOT_OPT_SIZE;
    ttr->max_fields = TUN_METADATA_NUM_OPTS;
    list_init(&ttr->mappings);

    for (i = 0; i < TUN_METADATA_NUM_OPTS; i++) {
        struct tun_meta_entry *entry = &map->entries[i];
        struct ofputil_tlv_map *map;

        if (!entry->valid) {
            continue;
        }

        map = xmalloc(sizeof *map);
        map->option_class = ntohs(tun_key_class(entry->key));
        map->option_type = tun_key_type(entry->key);
        map->option_len = entry->loc.len;
        map->index = i;

        list_push_back(&ttr->mappings, &map->list_node);
    }
}

/* Copies the value of field 'mf' from 'tnl' (which must be in non-UDPIF format) * into 'value'.
 *
 * 'mf' must be an MFF_TUN_METADATA* field.
 *
 * This uses the global tunnel metadata mapping table created by
 * tun_metadata_init().  If no such table has been created or if 'mf' hasn't
 * been allocated in it yet, this just zeros 'value'. */
void
tun_metadata_read(const struct flow_tnl *tnl,
                  const struct mf_field *mf, union mf_value *value)
{
    struct tun_table *map = ovsrcu_get(struct tun_table *, &metadata_tab);
    unsigned int idx = mf->id - MFF_TUN_METADATA0;
    struct tun_metadata_loc *loc;

    if (!map) {
        memset(value->tun_metadata, 0, mf->n_bytes);
        return;
    }

    loc = &map->entries[idx].loc;

    memset(value->tun_metadata, 0, mf->n_bytes - loc->len);
    memcpy_from_metadata(value->tun_metadata + mf->n_bytes - loc->len,
                         &tnl->metadata, loc);
}

/* Copies 'value' into field 'mf' in 'tnl' (in non-UDPIF format).
 *
 * 'mf' must be an MFF_TUN_METADATA* field.
 *
 * This uses the global tunnel metadata mapping table created by
 * tun_metadata_init().  If no such table has been created or if 'mf' hasn't
 * been allocated in it yet, this function does nothing. */
void
tun_metadata_write(struct flow_tnl *tnl,
                   const struct mf_field *mf, const union mf_value *value)
{
    struct tun_table *map = ovsrcu_get(struct tun_table *, &metadata_tab);
    unsigned int idx = mf->id - MFF_TUN_METADATA0;
    struct tun_metadata_loc *loc;

    if (!map || !map->entries[idx].valid) {
        return;
    }

    loc = &map->entries[idx].loc;
    memcpy_to_metadata(&tnl->metadata,
                       value->tun_metadata + mf->n_bytes - loc->len, loc, idx);
}

static const struct tun_metadata_loc *
metadata_loc_from_match(struct tun_table *map, struct match *match,
                        const char *name, unsigned int idx,
                        unsigned int field_len, bool masked, char **err_str)
{
    ovs_assert(idx < TUN_METADATA_NUM_OPTS);

    if (err_str) {
        *err_str = NULL;
    }

    if (map) {
        if (map->entries[idx].valid) {
            return &map->entries[idx].loc;
        } else {
            return NULL;
        }
    }

    if (match->tun_md.alloc_offset + field_len > TUN_METADATA_TOT_OPT_SIZE) {
        if (err_str) {
            *err_str = xasprintf("field %s exceeds maximum size for tunnel "
                                 "metadata (used %d, max %d)", name,
                                 match->tun_md.alloc_offset + field_len,
                                 TUN_METADATA_TOT_OPT_SIZE);
        }

        return NULL;
    }

    if (ULLONG_GET(match->wc.masks.tunnel.metadata.present.map, idx)) {
        if (err_str) {
            *err_str = xasprintf("field %s set multiple times", name);
        }

        return NULL;
    }

    match->tun_md.entry[idx].loc.len = field_len;
    match->tun_md.entry[idx].loc.c.offset = match->tun_md.alloc_offset;
    match->tun_md.entry[idx].loc.c.len = field_len;
    match->tun_md.entry[idx].loc.c.next = NULL;
    match->tun_md.entry[idx].masked = masked;
    match->tun_md.alloc_offset += field_len;
    match->tun_md.valid = true;

    return &match->tun_md.entry[idx].loc;
}

/* Makes 'match' match 'value'/'mask' on field 'mf'.
 *
 * 'mf' must be an MFF_TUN_METADATA* field. 'match' must be in non-UDPIF format.
 *
 * If there is global tunnel metadata matching table, this function is
 * effective only if there is already a mapping for 'mf'.  Otherwise, the
 * metadata mapping table integrated into 'match' is used, adding 'mf' to its
 * mapping table if it isn't already mapped (and if there is room).  If 'mf'
 * isn't or can't be mapped, this function returns without modifying 'match'.
 *
 * 'value' may be NULL; if so, then 'mf' is made to match on an all-zeros
 * value.
 *
 * 'mask' may be NULL; if so, then 'mf' is made exact-match.
 *
 * If non-NULL, 'err_str' returns a malloc'ed string describing any errors
 * with the request or NULL if there is no error. The caller is reponsible
 * for freeing the string.
 */
void
tun_metadata_set_match(const struct mf_field *mf, const union mf_value *value,
                       const union mf_value *mask, struct match *match,
                       char **err_str)
{
    struct tun_table *map = ovsrcu_get(struct tun_table *, &metadata_tab);
    const struct tun_metadata_loc *loc;
    unsigned int idx = mf->id - MFF_TUN_METADATA0;
    unsigned int field_len;
    bool is_masked;
    unsigned int data_offset;
    union mf_value data;

    ovs_assert(!(match->flow.tunnel.flags & FLOW_TNL_F_UDPIF));

    field_len = mf_field_len(mf, value, mask, &is_masked);
    loc = metadata_loc_from_match(map, match, mf->name, idx, field_len,
                                  is_masked, err_str);
    if (!loc) {
        return;
    }

    data_offset = mf->n_bytes - loc->len;

    if (!value) {
        memset(data.tun_metadata, 0, loc->len);
    } else if (!mask) {
        memcpy(data.tun_metadata, value->tun_metadata + data_offset, loc->len);
    } else {
        int i;
        for (i = 0; i < loc->len; i++) {
            data.tun_metadata[i] = value->tun_metadata[data_offset + i] &
                                   mask->tun_metadata[data_offset + i];
        }
    }
    memcpy_to_metadata(&match->flow.tunnel.metadata, data.tun_metadata,
                       loc, idx);

    if (!value) {
        memset(data.tun_metadata, 0, loc->len);
    } else if (!mask) {
        memset(data.tun_metadata, 0xff, loc->len);
    } else {
        memcpy(data.tun_metadata, mask->tun_metadata + data_offset, loc->len);
    }
    memcpy_to_metadata(&match->wc.masks.tunnel.metadata, data.tun_metadata,
                       loc, idx);
}

static bool
udpif_to_parsed(const struct flow_tnl *flow, const struct flow_tnl *mask,
                struct flow_tnl *flow_xlate, struct flow_tnl *mask_xlate)
{
    if (flow->flags & FLOW_TNL_F_UDPIF) {
        int err;

        err = tun_metadata_from_geneve_udpif(flow, flow, flow_xlate);
        if (err) {
            return false;
        }

        if (mask) {
            tun_metadata_from_geneve_udpif(flow, mask, mask_xlate);
            if (err) {
                return false;
            }
        }
    } else {
        if (flow->metadata.present.map == 0) {
            /* There is no tunnel metadata, don't bother copying. */
            return false;
        }

        memcpy(flow_xlate, flow, sizeof *flow_xlate);
        if (mask) {
            memcpy(mask_xlate, mask, sizeof *mask_xlate);
        }

        if (!flow_xlate->metadata.tab) {
            flow_xlate->metadata.tab = ovsrcu_get(struct tun_table *,
                                                  &metadata_tab);
        }
    }

    return true;
}

/* Copies all MFF_TUN_METADATA* fields from 'tnl' to 'flow_metadata'. */
void
tun_metadata_get_fmd(const struct flow_tnl *tnl, struct match *flow_metadata)
{
    struct flow_tnl flow;
    int i;

    if (!udpif_to_parsed(tnl, NULL, &flow, NULL)) {
        return;
    }

    ULLONG_FOR_EACH_1 (i, flow.metadata.present.map) {
        union mf_value opts;
        const struct tun_metadata_loc *old_loc = &flow.metadata.tab->entries[i].loc;
        const struct tun_metadata_loc *new_loc;

        new_loc = metadata_loc_from_match(NULL, flow_metadata, NULL, i,
                                          old_loc->len, false, NULL);

        memcpy_from_metadata(opts.tun_metadata, &flow.metadata, old_loc);
        memcpy_to_metadata(&flow_metadata->flow.tunnel.metadata,
                           opts.tun_metadata, new_loc, i);

        memset(opts.tun_metadata, 0xff, old_loc->len);
        memcpy_to_metadata(&flow_metadata->wc.masks.tunnel.metadata,
                           opts.tun_metadata, new_loc, i);
    }
}

static uint32_t
tun_meta_hash(uint32_t key)
{
    return hash_int(key, 0);
}

static struct tun_meta_entry *
tun_meta_find_key(const struct hmap *hmap, uint32_t key)
{
    struct tun_meta_entry *entry;

    HMAP_FOR_EACH_IN_BUCKET (entry, node, tun_meta_hash(key), hmap) {
        if (entry->key == key) {
            return entry;
        }
    }
    return NULL;
}

static void
memcpy_to_metadata(struct tun_metadata *dst, const void *src,
                   const struct tun_metadata_loc *loc, unsigned int idx)
{
    const struct tun_metadata_loc_chain *chain = &loc->c;
    int addr = 0;

    while (chain) {
        memcpy(dst->opts.u8 + loc->c.offset + addr, (uint8_t *)src + addr,
               chain->len);
        addr += chain->len;
        chain = chain->next;
    }

    ULLONG_SET1(dst->present.map, idx);
}

static void
memcpy_from_metadata(void *dst, const struct tun_metadata *src,
                     const struct tun_metadata_loc *loc)
{
    const struct tun_metadata_loc_chain *chain = &loc->c;
    int addr = 0;

    while (chain) {
        memcpy((uint8_t *)dst + addr, src->opts.u8 + loc->c.offset + addr,
               chain->len);
        addr += chain->len;
        chain = chain->next;
    }
}

static int
tun_metadata_alloc_chain(struct tun_table *map, uint8_t len,
                         struct tun_metadata_loc_chain *loc)
                         OVS_REQUIRES(tab_mutex)
{
    int alloc_len = len / 4;
    int scan_start = 0;
    int scan_end = TUN_METADATA_TOT_OPT_SIZE / 4;
    int pos_start, pos_end, pos_len;
    int best_start = 0, best_len = 0;

    while (true) {
        pos_start = bitmap_scan(map->alloc_map, 0, scan_start, scan_end);
        if (pos_start == scan_end) {
            break;
        }

        pos_end = bitmap_scan(map->alloc_map, 1, pos_start,
                              MIN(pos_start + alloc_len, scan_end));
        pos_len = pos_end - pos_start;
        if (pos_len == alloc_len) {
            goto found;
        }

        if (pos_len > best_len) {
            best_start = pos_start;
            best_len = pos_len;
        }
        scan_start = pos_end + 1;
    }

    if (best_len == 0) {
        return ENOSPC;
    }

    pos_start = best_start;
    pos_len = best_len;

found:
    bitmap_set_multiple(map->alloc_map, pos_start, pos_len, 1);
    loc->offset = pos_start * 4;
    loc->len = pos_len * 4;

    return 0;
}

static enum ofperr
tun_metadata_add_entry(struct tun_table *map, uint8_t idx, uint16_t opt_class,
                       uint8_t type, uint8_t len) OVS_REQUIRES(tab_mutex)
{
    struct tun_meta_entry *entry;
    struct tun_metadata_loc_chain *cur_chain, *prev_chain;

    ovs_assert(idx < TUN_METADATA_NUM_OPTS);

    entry = &map->entries[idx];
    if (entry->valid) {
        return OFPERR_NXTTMFC_ALREADY_MAPPED;
    }

    entry->key = tun_meta_key(htons(opt_class), type);
    if (tun_meta_find_key(&map->key_hmap, entry->key)) {
        return OFPERR_NXTTMFC_DUP_ENTRY;
    }

    entry->valid = true;
    hmap_insert(&map->key_hmap, &entry->node,
                tun_meta_hash(entry->key));

    entry->loc.len = len;
    cur_chain = &entry->loc.c;
    memset(cur_chain, 0, sizeof *cur_chain);
    prev_chain = NULL;

    while (len) {
        int err;

        if (!cur_chain) {
            cur_chain = xzalloc(sizeof *cur_chain);
        }

        err = tun_metadata_alloc_chain(map, len, cur_chain);
        if (err) {
            tun_metadata_del_entry(map, idx);
            return OFPERR_NXTTMFC_TABLE_FULL;
        }

        len -= cur_chain->len;

        if (prev_chain) {
            prev_chain->next = cur_chain;
        }
        prev_chain = cur_chain;
        cur_chain = NULL;
    }

    return 0;
}

static void
tun_metadata_del_entry(struct tun_table *map, uint8_t idx)
                       OVS_REQUIRES(tab_mutex)
{
    struct tun_meta_entry *entry;
    struct tun_metadata_loc_chain *chain;

    if (idx >= TUN_METADATA_NUM_OPTS) {
        return;
    }

    entry = &map->entries[idx];
    if (!entry->valid) {
        return;
    }

    chain = &entry->loc.c;
    while (chain) {
        struct tun_metadata_loc_chain *next = chain->next;

        bitmap_set_multiple(map->alloc_map, chain->offset / 4,
                            chain->len / 4, 0);
        if (chain != &entry->loc.c) {
            free(chain);
        }
        chain = next;
    }

    entry->valid = false;
    hmap_remove(&map->key_hmap, &entry->node);
    memset(&entry->loc, 0, sizeof entry->loc);
}

static int
tun_metadata_from_geneve__(const struct tun_metadata *flow_metadata,
                           const struct geneve_opt *opt,
                           const struct geneve_opt *flow_opt, int opts_len,
                           struct tun_metadata *metadata)
{
    struct tun_table *map;
    bool is_mask = flow_opt != opt;

    if (!is_mask) {
        map = ovsrcu_get(struct tun_table *, &metadata_tab);
        metadata->tab = map;
    } else {
        map = flow_metadata->tab;
    }

    if (!map) {
        return 0;
    }

    while (opts_len > 0) {
        int len;
        struct tun_meta_entry *entry;

        if (opts_len < sizeof(*opt)) {
            return EINVAL;
        }

        len = sizeof(*opt) + flow_opt->length * 4;
        if (len > opts_len) {
            return EINVAL;
        }

        entry = tun_meta_find_key(&map->key_hmap,
                                  tun_meta_key(flow_opt->opt_class,
                                               flow_opt->type));
        if (entry) {
            if (entry->loc.len == flow_opt->length * 4) {
                memcpy_to_metadata(metadata, opt + 1, &entry->loc,
                                   entry - map->entries);
            } else {
                return EINVAL;
            }
        } else if (flow_opt->type & GENEVE_CRIT_OPT_TYPE) {
            return EINVAL;
        }

        opt = opt + len / sizeof(*opt);
        flow_opt = flow_opt + len / sizeof(*opt);
        opts_len -= len;
    }

    return 0;
}

static const struct nlattr *
tun_metadata_find_geneve_key(const struct nlattr *key, uint32_t key_len)
{
    const struct nlattr *tnl_key;

    tnl_key = nl_attr_find__(key, key_len, OVS_KEY_ATTR_TUNNEL);
    if (!tnl_key) {
        return NULL;
    }

    return nl_attr_find_nested(tnl_key, OVS_TUNNEL_KEY_ATTR_GENEVE_OPTS);
}

/* Converts from Geneve netlink attributes in 'attr' to tunnel metadata
 * in 'tun'. The result may either in be UDPIF format or not, as determined
 * by 'udpif'.
 *
 * In the event that a mask is being converted, it is also necessary to
 * pass in flow information. This includes the full set of netlink attributes
 * (i.e. not just the Geneve attribute) in 'flow_attrs'/'flow_attr_len' and
 * the previously converted tunnel metadata 'flow_tun'.
 *
 * If a flow rather than mask is being converted, 'flow_attrs' must be NULL. */
int
tun_metadata_from_geneve_nlattr(const struct nlattr *attr,
                                const struct nlattr *flow_attrs,
                                size_t flow_attr_len,
                                const struct flow_tnl *flow_tun, bool udpif,
                                struct flow_tnl *tun)
{
    bool is_mask = !!flow_attrs;
    int attr_len = nl_attr_get_size(attr);
    const struct nlattr *flow;

    /* No need for real translation, just copy things over. */
    if (udpif) {
        memcpy(tun->metadata.opts.gnv, nl_attr_get(attr), attr_len);

        if (!is_mask) {
            tun->metadata.present.len = attr_len;
            tun->flags |= FLOW_TNL_F_UDPIF;
        } else {
            /* We need to exact match on the length so we don't
             * accidentally match on sets of options that are the same
             * at the beginning but with additional options after. */
            tun->metadata.present.len = 0xff;
        }

        return 0;
    }

    if (is_mask) {
        flow = tun_metadata_find_geneve_key(flow_attrs, flow_attr_len);
        if (!flow) {
            return attr_len ? EINVAL : 0;
        }

        if (attr_len != nl_attr_get_size(flow)) {
            return EINVAL;
        }
    } else {
        flow = attr;
    }

    return tun_metadata_from_geneve__(&flow_tun->metadata, nl_attr_get(attr),
                                      nl_attr_get(flow), nl_attr_get_size(flow),
                                      &tun->metadata);
}

/* Converts from the flat Geneve options representation extracted directly
 * from the tunnel header to the representation that maps options to
 * pre-allocated locations. The original version (in UDPIF form) is passed
 * in 'src' and the translated form in stored in 'dst'.  To handle masks, the
 * flow must also be passed in through 'flow' (in the original, raw form). */
int
tun_metadata_from_geneve_udpif(const struct flow_tnl *flow,
                               const struct flow_tnl *src,
                               struct flow_tnl *dst)
{
    ovs_assert(flow->flags & FLOW_TNL_F_UDPIF);

    if (flow == src) {
        dst->flags = flow->flags & ~FLOW_TNL_F_UDPIF;
    } else {
        dst->metadata.tab = NULL;
    }
    dst->metadata.present.map = 0;
    return tun_metadata_from_geneve__(&flow->metadata, src->metadata.opts.gnv,
                                      flow->metadata.opts.gnv,
                                      flow->metadata.present.len,
                                      &dst->metadata);
}

static void
tun_metadata_to_geneve__(const struct tun_metadata *flow, struct ofpbuf *b,
                         bool *crit_opt)
{
    struct tun_table *map;
    int i;

    map = flow->tab;
    if (!map) {
        map = ovsrcu_get(struct tun_table *, &metadata_tab);
    }

    *crit_opt = false;

    ULLONG_FOR_EACH_1 (i, flow->present.map) {
        struct tun_meta_entry *entry = &map->entries[i];
        struct geneve_opt *opt;

        opt = ofpbuf_put_uninit(b, sizeof *opt + entry->loc.len);

        opt->opt_class = tun_key_class(entry->key);
        opt->type = tun_key_type(entry->key);
        opt->length = entry->loc.len / 4;
        opt->r1 = 0;
        opt->r2 = 0;
        opt->r3 = 0;

        memcpy_from_metadata(opt + 1, flow, &entry->loc);
        *crit_opt |= !!(opt->type & GENEVE_CRIT_OPT_TYPE);
    }
}

static void
tun_metadata_to_geneve_nlattr_flow(const struct flow_tnl *flow,
                                   struct ofpbuf *b)
{
    size_t nlattr_offset;
    bool crit_opt;

    if (!flow->metadata.present.map) {
        return;
    }

    /* For all intents and purposes, the Geneve options are nested
     * attributes even if this doesn't show up directly to netlink. It's
     * similar enough that we can use the same mechanism. */
    nlattr_offset = nl_msg_start_nested(b, OVS_TUNNEL_KEY_ATTR_GENEVE_OPTS);

    tun_metadata_to_geneve__(&flow->metadata, b, &crit_opt);

    nl_msg_end_nested(b, nlattr_offset);
}

/* Converts from processed tunnel metadata information (in non-udpif
 * format) in 'flow' to a stream of Geneve options suitable for
 * transmission in 'opts'. Additionally returns whether there were
 * any critical options in 'crit_opt' as well as the total length of
 * data. */
int
tun_metadata_to_geneve_header(const struct flow_tnl *flow,
                              struct geneve_opt *opts, bool *crit_opt)
{
    struct ofpbuf b;

    ovs_assert(!(flow->flags & FLOW_TNL_F_UDPIF));

    ofpbuf_use_stack(&b, opts, TLV_TOT_OPT_SIZE);
    tun_metadata_to_geneve__(&flow->metadata, &b, crit_opt);

    return b.size;
}

static void
tun_metadata_to_geneve_mask__(const struct tun_metadata *flow,
                              const struct tun_metadata *mask,
                              struct geneve_opt *opt, int opts_len)
{
    struct tun_table *map = flow->tab;

    if (!map) {
        return;
    }

    /* All of these options have already been validated, so no need
     * for sanity checking. */
    while (opts_len > 0) {
        struct tun_meta_entry *entry;
        int len = sizeof(*opt) + opt->length * 4;

        entry = tun_meta_find_key(&map->key_hmap,
                                  tun_meta_key(opt->opt_class, opt->type));
        if (entry) {
            memcpy_from_metadata(opt + 1, mask, &entry->loc);
        } else {
            memset(opt + 1, 0, opt->length * 4);
        }

        opt->opt_class = htons(0xffff);
        opt->type = 0xff;
        opt->length = 0x1f;
        opt->r1 = 0;
        opt->r2 = 0;
        opt->r3 = 0;

        opt = opt + len / sizeof(*opt);
        opts_len -= len;
    }
}

static void
tun_metadata_to_geneve_nlattr_mask(const struct ofpbuf *key,
                                   const struct flow_tnl *mask,
                                   const struct flow_tnl *flow,
                                   struct ofpbuf *b)
{
    const struct nlattr *geneve_key;
    struct nlattr *geneve_mask;
    struct geneve_opt *opt;
    int opts_len;

    if (!key) {
        return;
    }

    geneve_key = tun_metadata_find_geneve_key(key->data, key->size);
    if (!geneve_key) {
        return;
    }

    geneve_mask = ofpbuf_tail(b);
    nl_msg_put(b, geneve_key, geneve_key->nla_len);

    opt = CONST_CAST(struct geneve_opt *, nl_attr_get(geneve_mask));
    opts_len = nl_attr_get_size(geneve_mask);

    tun_metadata_to_geneve_mask__(&flow->metadata, &mask->metadata,
                                  opt, opts_len);
}

/* Convert from the tunnel metadata in 'tun' to netlink attributes stored
 * in 'b'. Either UDPIF or non-UDPIF input forms are accepted.
 *
 * To assist with parsing, it is necessary to also pass in the tunnel metadata
 * from the flow in 'flow' as well in the original netlink form of the flow in
 * 'key'. */
void
tun_metadata_to_geneve_nlattr(const struct flow_tnl *tun,
                              const struct flow_tnl *flow,
                              const struct ofpbuf *key,
                              struct ofpbuf *b)
{
    bool is_mask = tun != flow;

    if (!(flow->flags & FLOW_TNL_F_UDPIF)) {
        if (!is_mask) {
            tun_metadata_to_geneve_nlattr_flow(tun, b);
        } else {
            tun_metadata_to_geneve_nlattr_mask(key, tun, flow, b);
        }
    } else if (flow->metadata.present.len || is_mask) {
        nl_msg_put_unspec(b, OVS_TUNNEL_KEY_ATTR_GENEVE_OPTS,
                          tun->metadata.opts.gnv,
                          flow->metadata.present.len);
    }
}

/* Converts 'mask_src' (in non-UDPIF format) to a series of masked options in
 * 'dst'. 'flow_src' (also in non-UDPIF format) and the  original set of
 * options 'flow_src_opt'/'opts_len' are needed as a guide to interpret the
 * mask data. */
void
tun_metadata_to_geneve_udpif_mask(const struct flow_tnl *flow_src,
                                  const struct flow_tnl *mask_src,
                                  const struct geneve_opt *flow_src_opt,
                                  int opts_len, struct geneve_opt *dst)
{
    ovs_assert(!(flow_src->flags & FLOW_TNL_F_UDPIF));

    memcpy(dst, flow_src_opt, opts_len);
    tun_metadata_to_geneve_mask__(&flow_src->metadata,
                                  &mask_src->metadata, dst, opts_len);
}

static const struct tun_metadata_loc *
metadata_loc_from_match_read(struct tun_table *map, const struct match *match,
                             unsigned int idx, struct flow_tnl *mask,
                             bool *is_masked)
{
    union mf_value mask_opts;

    if (match->tun_md.valid) {
        *is_masked = match->tun_md.entry[idx].masked;
        return &match->tun_md.entry[idx].loc;
    }

    memcpy_from_metadata(mask_opts.tun_metadata, &mask->metadata,
                         &map->entries[idx].loc);

    *is_masked = map->entries[idx].loc.len == 0 ||
                 !is_all_ones(mask_opts.tun_metadata,
                              map->entries[idx].loc.len);
    return &map->entries[idx].loc;
}

void
tun_metadata_to_nx_match(struct ofpbuf *b, enum ofp_version oxm,
                         const struct match *match)
{
    struct flow_tnl flow, mask;
    int i;

    if (!udpif_to_parsed(&match->flow.tunnel, &match->wc.masks.tunnel,
                         &flow, &mask)) {
        return;
    }

    ULLONG_FOR_EACH_1 (i, mask.metadata.present.map) {
        const struct tun_metadata_loc *loc;
        bool is_masked;
        union mf_value opts;
        union mf_value mask_opts;

        loc = metadata_loc_from_match_read(flow.metadata.tab, match, i,
                                           &mask, &is_masked);
        memcpy_from_metadata(opts.tun_metadata, &flow.metadata, loc);
        memcpy_from_metadata(mask_opts.tun_metadata, &mask.metadata, loc);
        nxm_put__(b, MFF_TUN_METADATA0 + i, oxm, opts.tun_metadata,
                  is_masked ? mask_opts.tun_metadata : NULL, loc->len);
    }
}

void
tun_metadata_match_format(struct ds *s, const struct match *match)
{
    struct flow_tnl flow, mask;
    unsigned int i;

    if (!udpif_to_parsed(&match->flow.tunnel, &match->wc.masks.tunnel,
                         &flow, &mask)) {
        return;
    }

    ULLONG_FOR_EACH_1 (i, mask.metadata.present.map) {
        const struct tun_metadata_loc *loc;
        bool is_masked;
        union mf_value opts, mask_opts;

        loc = metadata_loc_from_match_read(flow.metadata.tab, match, i,
                                           &mask, &is_masked);

        ds_put_format(s, "tun_metadata%u", i);
        memcpy_from_metadata(mask_opts.tun_metadata, &mask.metadata, loc);

        if (!ULLONG_GET(flow.metadata.present.map, i)) {
            /* Indicate that we are matching on the field being not present. */
            ds_put_cstr(s, "=NP");
        } else if (!(is_masked &&
                     is_all_zeros(mask_opts.tun_metadata, loc->len))) {
            ds_put_char(s, '=');

            memcpy_from_metadata(opts.tun_metadata, &flow.metadata, loc);
            ds_put_hex(s, opts.tun_metadata, loc->len);

            if (!is_all_ones(mask_opts.tun_metadata, loc->len)) {
                ds_put_char(s, '/');
                ds_put_hex(s, mask_opts.tun_metadata, loc->len);
            }
        }
        ds_put_char(s, ',');
    }
}
