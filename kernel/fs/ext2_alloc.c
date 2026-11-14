/* arc_os — ext2 allocation: bitmaps, counts and the metadata write order
 * (ext2_internal.h, M1-15)
 *
 * Owns: taking and giving back blocks and inode numbers in the group
 * bitmaps, the per-group and total free counts (Ext2Vol.grp, free_blocks,
 * free_inodes: the authority while mounted), copying them into the group
 * descriptors and the superblock (a commit), and the order the metadata
 * cache is written in.
 *
 * Where things go: a block near the goal the caller gives (the block after
 * the file's previous one, so a file written in order lies in order),
 * searching on from the goal through its group, then the following groups,
 * then the goal's group from its start. A directory's inode goes to a group
 * with at least the average free inodes and blocks, taking the groups in
 * turn (dir_rotor), so directory trees spread over the disk; a file's inode
 * to its directory's group, else the next group with room.
 *
 * Write order (spec: no journal, so data, then the inode, then the bitmaps):
 * the write-back pass writes every file's data and waits, flushes the disk,
 * then calls ext2_meta_writepages, which commits the counts and writes the
 * metadata in two rounds: first everything but the allocation state (inode
 * tables, indirect blocks, directories) and waits, flushes (always: the wait
 * may cover writes others started), then the
 * allocation state (block and inode bitmaps, group descriptors, superblock).
 * A crash between rounds leaves blocks or inodes in use that the bitmaps
 * call free (e2fsck fixes the bitmaps; nothing points at garbage). Freeing
 * made before a pass starts runs the other way round in effect: pointers are
 * cleared (round one) before the bits (round two), so a crash between leaks
 * blocks. New indirect and directory blocks are written before any pointer
 * to them exists (ext2_meta_new_done). Not covered, all repaired by e2fsck
 * (M5-01): a free made while a pass runs (another thread's truncate or
 * unlink after round one started), whose cleared bits round two writes
 * before the cleared pointers (the next pass), so a crash between leaves an
 * inode pointing at a block its bitmap calls free; kswapd writing pages
 * alone to free memory (a metadata page in any order, or file data still in
 * flight when a pass writes the inode that points at it: stale bytes after
 * a crash); and a freed block reused before
 * the inode that freed it reaches the disk. fsync writes the whole volume's metadata after its
 * own file's data only, so another file's new block pointers may reach the
 * disk before that file's data: after a crash it can show stale bytes
 * (Linux ext2 alike; ordered data is a journal's job).
 *
 * Not done: the reserved blocks (s_r_blocks_count) are not kept back for
 * root; the backup superblocks and descriptors in other groups are left as
 * mkfs wrote them (Linux ext2 does the same).
 *
 * Locks: everything here runs with alloc_lock held (LOCK_LEVEL_FS_ALLOC),
 * locking one bitmap, descriptor or superblock page at a time.
 *
 * Cost: a block allocation is one bitmap page lock and a search that stops
 * at the first free bit after the goal (64 bits per step); a full group
 * search is blocks_per_group / 64 steps. Freeing is one bitmap page per run
 * of blocks in one group. A commit locks one descriptor page per changed
 * group. */

#include "fs/ext2_internal.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

static Ext2Vol *vol_of_meta(AddressSpace *as) {
    return (Ext2Vol *)((uint8_t *)as - __builtin_offsetof(Ext2Vol, meta));
}

/* Blocks in group g (the last group may be short) */
static uint32_t group_blocks(const Ext2Vol *v, uint32_t g) {
    uint64_t left = (uint64_t)v->blocks_count - v->first_data_block - (uint64_t)g * v->blocks_per_group;
    return left < v->blocks_per_group ? (uint32_t)left : v->blocks_per_group;
}

/* Inodes in group g */
static uint32_t group_inodes(const Ext2Vol *v, uint32_t g) {
    uint64_t left = (uint64_t)v->inodes_count - (uint64_t)g * v->inodes_per_group;
    return left < v->inodes_per_group ? (uint32_t)left : v->inodes_per_group;
}

static void group_changed(Ext2Vol *v, uint32_t g) {
    v->grp_dirty[g / 64] |= 1ull << (g % 64);
    v->super_dirty = 1;
}

/* The first clear bit in [from, n) of map, or n */
static uint32_t find_zero(const uint8_t *map, uint32_t from, uint32_t n) {
    uint32_t i = from;
    for (; i < n && (i & 63) != 0; i++) {
        if (!(map[i >> 3] & (1u << (i & 7)))) return i;
    }
    for (; i + 64 <= n; i += 64) {
        uint64_t w;
        memcpy(&w, map + (i >> 3), sizeof(w));
        if (w != ~0ull) return i + (uint32_t)__builtin_ctzll(~w);
    }
    for (; i < n; i++) {
        if (!(map[i >> 3] & (1u << (i & 7)))) return i;
    }
    return n;
}

/* Find a clear bit in [from, n) of the bitmap in block `bitmap`, set it:
 * the bit, n if none is clear, or -errno */
static int64_t take_bit(Ext2Vol *v, uint32_t bitmap, uint32_t from, uint32_t n) {
    struct page *pg;
    int err;
    uint8_t *map = ext2_meta_lock(v, ext2_block_at(v, bitmap), &pg, &err);
    if (map == NULL) return err;
    uint32_t bit = find_zero(map, from, n);
    if (bit < n) map[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    ext2_meta_done(pg, bit < n);
    return bit;
}

/* Clear count bits from first in the bitmap in block `bitmap`: how many
 * were set, or -errno */
static int64_t clear_bits(Ext2Vol *v, uint32_t bitmap, uint32_t first, uint32_t count) {
    struct page *pg;
    int err;
    uint8_t *map = ext2_meta_lock(v, ext2_block_at(v, bitmap), &pg, &err);
    if (map == NULL) return err;
    uint32_t was = 0;
    for (uint32_t i = first; i < first + count; i++) {
        uint8_t m = (uint8_t)(1u << (i & 7));
        if (map[i >> 3] & m) was++;
        map[i >> 3] &= (uint8_t)~m;
    }
    ext2_meta_done(pg, was != 0);
    return was;
}

/* Is block b one of group g's own bitmaps or inode table? */
static int is_group_meta(const Ext2Vol *v, uint32_t g, uint32_t b) {
    const Ext2Group *gr = &v->grp[g];
    uint32_t table = (uint32_t)((uint64_t)v->inodes_per_group * v->inode_size >> v->block_shift);
    return b == gr->block_bitmap || b == gr->inode_bitmap || (b >= gr->inode_table && b - gr->inode_table < table);
}

int ext2_alloc_block(Ext2Vol *v, uint32_t goal, uint32_t *out) {
    if (v->free_blocks == 0) return -ENOSPC;
    uint32_t first = v->first_data_block;
    if (goal < first || goal >= v->blocks_count) goal = first;
    uint32_t g0 = (goal - first) / v->blocks_per_group;
    uint32_t bit0 = (goal - first) % v->blocks_per_group;
    /* The goal's group from the goal, the others, the goal's group again */
    for (uint32_t i = 0; i <= v->groups; i++) {
        uint32_t g = (g0 + i) % v->groups;
        Ext2Group *gr = &v->grp[g];
        if (gr->free_blocks == 0) continue;
        uint32_t n = group_blocks(v, g);
        int64_t bit = take_bit(v, gr->block_bitmap, i == 0 ? bit0 : 0, n);
        if (bit < 0) return (int)bit;
        if (bit == n) {
            if (i != 0) (void)ext2_corrupt(v, "free block count of group", g);   /* says free, has none */
            continue;
        }
        uint32_t b = first + g * v->blocks_per_group + (uint32_t)bit;
        if (is_group_meta(v, g, b)) return ext2_corrupt(v, "block bitmap (metadata block free)", b);
        gr->free_blocks--;
        v->free_blocks--;
        group_changed(v, g);
        *out = b;
        return 0;
    }
    return -ENOSPC;
}

void ext2_free_blocks(Ext2Vol *v, uint32_t first, uint32_t count, int meta) {
    while (count > 0) {
        if (first <= v->first_data_block || first >= v->blocks_count || count > v->blocks_count - first) {
            (void)ext2_corrupt(v, "block freed", first);
            return;
        }
        uint32_t g = (first - v->first_data_block) / v->blocks_per_group;
        uint32_t bit = (first - v->first_data_block) % v->blocks_per_group;
        uint32_t n = count < v->blocks_per_group - bit ? count : v->blocks_per_group - bit;
        Ext2Group *gr = &v->grp[g];
        int64_t was = clear_bits(v, gr->block_bitmap, bit, n);
        if (was < 0) {
            kprintf("[EXT2] %s: block bitmap of group %u unreadable: %u blocks leak\n", v->name, g, n);
        } else {
            if ((uint32_t)was != n) (void)ext2_corrupt(v, "free of a free block", first);
            gr->free_blocks += (uint32_t)was;
            v->free_blocks += (uint32_t)was;
            group_changed(v, g);
        }
        /* A freed metadata block's cached page must never be written over
         * whatever the block holds next (4 KiB blocks: block = page) */
        for (uint32_t i = 0; meta && i < n; i++) filemap_drop_page(&v->meta, ext2_block_at(v, first + i) >> PAGE_SHIFT);
        first += n;
        count -= n;
    }
}

/* A group with free inodes for a new inode */
static uint32_t pick_group(Ext2Vol *v, uint32_t parent_ino, int dir) {
    uint32_t groups = v->groups;
    if (dir) {
        uint32_t avg_i = v->free_inodes / groups, avg_b = v->free_blocks / groups;
        for (uint32_t i = 0; i < groups; i++) {
            uint32_t g = (v->dir_rotor + i) % groups;
            const Ext2Group *gr = &v->grp[g];
            if (gr->free_inodes > 0 && gr->free_inodes >= avg_i && gr->free_blocks >= avg_b) {
                v->dir_rotor = g + 1;
                return g;
            }
        }
    } else {
        uint32_t pg = (parent_ino - 1) / v->inodes_per_group;
        for (uint32_t i = 0; i < groups; i++) {
            uint32_t g = (pg + i) % groups;
            if (v->grp[g].free_inodes > 0 && (v->grp[g].free_blocks > 0 || i == 0)) return g;
        }
    }
    for (uint32_t g = 0; g < groups; g++) {
        if (v->grp[g].free_inodes > 0) return g;
    }
    return 0;
}

int ext2_alloc_ino(Ext2Vol *v, uint32_t parent_ino, int dir, uint32_t *out) {
    if (v->free_inodes == 0) return -ENOSPC;
    uint32_t g0 = pick_group(v, parent_ino, dir);
    for (uint32_t i = 0; i < v->groups; i++) {
        uint32_t g = (g0 + i) % v->groups;
        Ext2Group *gr = &v->grp[g];
        if (gr->free_inodes == 0) continue;
        uint32_t from = g == 0 ? v->first_ino - 1 : 0;   /* inodes 1 .. first_ino - 1 are reserved */
        uint32_t n = group_inodes(v, g);
        if (from >= n) continue;
        int64_t bit = take_bit(v, gr->inode_bitmap, from, n);
        if (bit < 0) return (int)bit;
        if (bit == n) {
            (void)ext2_corrupt(v, "free inode count of group", g);
            continue;
        }
        gr->free_inodes--;
        v->free_inodes--;
        if (dir) gr->used_dirs++;
        group_changed(v, g);
        *out = g * v->inodes_per_group + (uint32_t)bit + 1;
        return 0;
    }
    return -ENOSPC;
}

void ext2_free_ino(Ext2Vol *v, uint32_t ino, int dir) {
    if (ino < v->first_ino || ino > v->inodes_count) {
        (void)ext2_corrupt(v, "inode freed", ino);
        return;
    }
    uint32_t g = (ino - 1) / v->inodes_per_group;
    Ext2Group *gr = &v->grp[g];
    int64_t was = clear_bits(v, gr->inode_bitmap, (ino - 1) % v->inodes_per_group, 1);
    if (was != 1) {
        (void)ext2_corrupt(v, "free of a free inode", ino);
        return;
    }
    gr->free_inodes++;
    v->free_inodes++;
    if (dir && gr->used_dirs > 0) gr->used_dirs--;
    group_changed(v, g);
}

void ext2_commit(Ext2Vol *v) {
    uint64_t gdt = ext2_block_at(v, v->first_data_block + 1);
    for (uint32_t w = 0; w < (v->groups + 63) / 64; w++) {
        while (v->grp_dirty[w] != 0) {
            uint32_t g = w * 64 + (uint32_t)__builtin_ctzll(v->grp_dirty[w]);
            struct page *pg;
            int err;
            Ext2DiskGroup *gd =
                (Ext2DiskGroup *)ext2_meta_lock(v, gdt + (uint64_t)g * sizeof(Ext2DiskGroup), &pg, &err);
            if (gd == NULL) {
                kprintf("[EXT2] %s: group descriptor %u not written (%d)\n", v->name, g, err);
                return;   /* the bits stay: the next commit tries again */
            }
            gd->bg_free_blocks_count = (uint16_t)v->grp[g].free_blocks;
            gd->bg_free_inodes_count = (uint16_t)v->grp[g].free_inodes;
            gd->bg_used_dirs_count = (uint16_t)v->grp[g].used_dirs;
            ext2_meta_done(pg, 1);
            v->grp_dirty[w] &= ~(1ull << (g % 64));
        }
    }
    if (!__atomic_load_n(&v->super_dirty, __ATOMIC_RELAXED)) return;
    struct page *pg;
    int err;
    Ext2DiskSuper *s = (Ext2DiskSuper *)ext2_meta_lock(v, EXT2_SUPER_OFFSET, &pg, &err);
    if (s == NULL) return;
    __atomic_store_n(&v->super_dirty, 0, __ATOMIC_RELAXED);
    s->s_free_blocks_count = v->free_blocks;
    s->s_free_inodes_count = v->free_inodes;
    s->s_feature_ro_compat = __atomic_load_n(&v->ro_compat, __ATOMIC_RELAXED);
    ext2_meta_done(pg, 1);
}

/* Is metadata page index part of the allocation state (superblock, group
 * descriptors, a bitmap)? Writable volumes only: 4 KiB blocks, block = page */
static int alloc_state(void *ctx, uint64_t index) {
    const Ext2Vol *v = ctx;
    if (index >= v->blocks_count) return 0;
    uint32_t b = (uint32_t)index;
    if (b == 0 || (b > v->first_data_block && b <= v->first_data_block + v->gdt_blocks)) return 1;
    if (b < v->first_data_block) return 0;
    uint32_t g = (b - v->first_data_block) / v->blocks_per_group;
    return g < v->groups && (v->grp[g].block_bitmap == b || v->grp[g].inode_bitmap == b);
}

static int round_one(void *ctx, uint64_t index) {
    return !alloc_state(ctx, index);
}

int ext2_meta_writepages(AddressSpace *as) {
    Ext2Vol *v = vol_of_meta(as);
    if (!v->writable) return 0;   /* nothing is ever dirty */
    mutex_lock(&v->alloc_lock);
    ext2_commit(v);
    mutex_unlock(&v->alloc_lock);
    int err = filemap_start_range_if(as, 0, UINT64_MAX, round_one, v);
    int w = filemap_wait_range_if(as, 0, UINT64_MAX, round_one, v);
    if (err == 0) err = w;
    /* Round one on the medium before round two: always, since the wait may
     * have been for writes someone else started (kswapd, fsync) even when
     * this round started none */
    int f = filemap_flush_device(as);
    if (err == 0) err = f;
    int e2 = filemap_start_range_if(as, 0, UINT64_MAX, alloc_state, v);
    return err != 0 ? err : e2;
}

uint64_t ext2_max_size(uint32_t bits) {
    /* i_blocks counts 512-byte units in 32 bits, indirect blocks included */
    uint64_t per = 1ull << (bits - 2);
    uint64_t upper = ((1ull << 32) - 1) >> (bits - 9);
    uint64_t meta = 1 + (1 + per) + (1 + per + per * per);
    upper = (upper - meta) << bits;
    uint64_t map = (EXT2_NDIR_BLOCKS + per + per * per + per * per * per) << bits;
    return map < upper ? map : upper;
}
