// TFS3's journal: the fixed-size multi-block transaction, the DEFERRED
// transaction storage.sync=batched uses, and staging an inode through
// it. Split out of tfs3.c; tfs3_internal.h has the map.

#include "tfs3_internal.h"
#include "ktime.h"
#include "caltime.h"
#include "string.h"
#include "storage_config.h" // storage.sync -- whether the barriers are real
#include "klog.h"

// ---- journal: one fixed-size multi-block transaction ---------------------
//
// What goes THROUGH the journal: dirent blocks and inode-table blocks
// -- the structures whose torn write is NAMESPACE corruption. What
// deliberately does NOT: bitmap and GDT blocks (leak-safe ordering
// above; a torn bitmap costs a leak fsck reclaims), data blocks and
// indirect-pointer blocks (unreachable until the inode that points at
// them commits). This is narrower than the design doc's first sketch
// ("dirent + inode + bitmaps") and is a deliberate revision: bitmaps
// under the leak rule keeps every operation's transaction at <= 3
// slots with the same crash guarantees TFS2 gives, for half the
// journaled bytes. The doc's journal section records this.
//
// Commit discipline = persist_record()'s, generalized (see TFS2 for
// the two-barrier reasoning): stage images + committed header, FLUSH,
// write targets, FLUSH, clear header (no barrier -- a stale committed
// header just replays idempotently).

// A DEFERRED transaction is never reset here: it holds other operations'
// completed writes, and only t3_txn_flush_deferred() -- which clears the
// flag first -- may commit or drop it. Every other caller that resets
// on failure is unwinding its OWN transaction, and after a refused
// t3_txn_begin() it has none.
void t3_txn_reset(struct t3_state *sbi) {
    if (sbi->txn_deferred) return;
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
}

// Open a transaction that will stage at most `credits` DISTINCT blocks,
// jbd2's reservation discipline in miniature: an operation that cannot
// fit says so before it has changed anything, instead of discovering it
// halfway through when t3_txn_stage() returns 0 and every caller has to
// unwind by hand. Returns 0 if this volume's journal is too small --
// which is the whole reason it exists, since a v1 image (four slots)
// genuinely cannot express a directory move. Callers turn that into a
// refusal with an explanation, not a corrupt half-operation.
// ---- the DEFERRED transaction (storage.sync = batched) ---------------
//
// Under `batched` a write stages its inode and does NOT commit; many
// writes then share one commit, which is what removes the barriers that
// are 56-69% of block time on real hardware. What is deferred is only
// the INODE UPDATE: do_write_inner() has already written the data
// blocks and the allocation bitmaps by the time it opens a transaction,
// so a crash loses a size update and leaks the blocks past it -- a leak
// `fsck` reclaims, not a journal that cannot be replayed.
//
// THE HAZARD IS t3_txn_begin() ITSELF. It zeroes txn_count, so any
// operation that opened a transaction while one was deferred would
// silently discard every inode staged in it -- writes reported as
// succeeded, vanishing. So the commit is forced HERE, in the one
// function every transaction in this file goes through, rather than at
// the call sites: create, delete, rename, truncate and the rest are
// covered without being edited, and a new one cannot forget.
//
// The journal is PER MOUNT, so a deferred transaction is always this
// volume's own; no other mount can find it half-staged.

// Commit whatever is deferred on this mount. Safe to call with nothing
// open. Returns 0 only if the commit itself failed.
int t3_txn_flush_deferred(struct t3_state *sbi) {
    if (!sbi->txn_deferred) return 1;
    int count = sbi->txn_count, credits = sbi->txn_credits;
    sbi->txn_deferred = 0;
    // The allocation state the batch accumulated goes first, BEFORE the
    // commit that makes those blocks reachable -- do_write_inner() skips
    // it per write under `batched`, which is the whole saving.
    int ok = t3_flush_alloc_state(sbi) && t3_txn_commit(sbi);
    if (!ok) {
        // KEEP THE STAGED WORK. t3_txn_commit() zeroed the count, but the
        // images are intact and the targets are either untouched or
        // left committed for replay, so the next flush can retry.
        // Dropping them here lost another process's completed writes
        // whenever an unrelated operation failed its journal write:
        // blocks allocated, inode never updated, a leak at fsck.
        sbi->txn_count = count;
        sbi->txn_credits = credits;
        sbi->txn_deferred = 1;
    }
    return ok;
}

int t3_txn_begin(struct t3_state *sbi, int credits) {
    // See the hazard note above: never discard staged work -- and a
    // flush that FAILS keeps it, so the new operation is refused rather
    // than staged on top of it.
    if (sbi->txn_deferred && !t3_txn_flush_deferred(sbi)) return 0;
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
    if (credits <= 0 || credits > (int)sbi->jslots) return 0;
    sbi->txn_credits = credits;
    return 1;
}

// Stage `blk`'s new image into the transaction. Returns a writable
// pointer to the staged 4 KiB image (pre-loaded from disk so callers
// patch in place), or 0 when full/read-failed. Staging the same block
// twice returns the same image.
uint8_t *t3_txn_stage(struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) {
        if (sbi->txn_target[i] == blk) return sbi->txn_img[i];
    }
    // Past the reservation is a bug in the CALLER's credit count, not
    // a runtime condition -- but refusing here keeps it a failed
    // operation rather than a torn one.
    if (sbi->txn_count >= sbi->txn_credits || sbi->txn_count >= (int)sbi->jslots) return 0;
    if (!t3_read_block(sbi, blk, sbi->txn_img[sbi->txn_count])) return 0;
    sbi->txn_target[sbi->txn_count] = blk;
    return sbi->txn_img[sbi->txn_count++];
}

// The header's slot table has to move for v2: four v1 entries end at
// byte 44, which is exactly where v1 put the header checksum, so 32 of
// them would overwrite it. v2 starts the table at 16 and puts the
// checksum in the sector's last four bytes -- room for 60 slots before
// the two would meet again. The version decides which shape is
// written and read; nothing guesses.
#define T3_JH_V1_SLOTS_OFF  12u
#define T3_JH_V1_CKSUM_OFF  44u
#define T3_JH_V2_SLOTS_OFF  16u
#define T3_JH_V2_CKSUM_OFF  508u   // ON-DISK: never derive it from a sector size

static uint32_t jh_slots_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_SLOTS_OFF : T3_JH_V1_SLOTS_OFF;
}
uint32_t t3_jh_cksum_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_CKSUM_OFF : T3_JH_V1_CKSUM_OFF;
}

static int write_journal_header(struct t3_state *sbi, int commit) {
    uint8_t sec[T3_SECTOR];
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = t3_jh_cksum_off(sbi->sb.version);
    k_memset(sec, 0, sizeof(sec));
    sec[0] = 'J'; sec[1] = 'R'; sec[2] = 'N'; sec[3] = '3';
    sec[4] = (uint8_t)commit;
    sec[5] = (uint8_t)sbi->txn_count;
    wr32(sec + 8, sbi->jrn_seq);
    for (int i = 0; i < sbi->txn_count; i++) {
        wr32(sec + slots + (uint32_t)i * 8, sbi->txn_target[i]);
        wr32(sec + slots + (uint32_t)i * 8 + 4, k_fnv1a(sbi->txn_img[i], T3_BLOCK));
    }
    wr32(sec + ck, k_fnv1a(sec, ck));
    return t3_vol_write_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec);
}

// A journal barrier, or a no-op under `storage.sync = lazy`.
//
// BOTH of t3_txn_commit()'s barriers go through here, because neither is
// the optional one: the first orders the journal against the targets so
// a crash mid-target-write can be replayed, the second orders the
// targets against clearing the commit flag so a crash cannot leave the
// journal saying "nothing to do" over work that never landed. Turning
// them off trades crash recoverability for throughput, which is ext4's
// `nobarrier` exactly -- see storage_config.c for what it costs and why
// the default is the other one.
//
// It still returns 1 when skipped. A caller must not read "no barrier
// was issued" as "the barrier failed": failure ABANDONS the
// transaction, and doing that on every write would be a filesystem that
// refuses to write at all.
static int txn_barrier(struct t3_state *sbi) {
    if (!storage_sync_strict()) return 1;
    return blkdev_flush(sbi->vol.dev);
}

// A COMMIT HAS THREE OUTCOMES, AND THE THIRD IS THE POINT.
//
// Past the commit point the journal is durable and WILL be replayed,
// so the blocks the operation allocated are live even though the
// operation failed -- freeing them hands a replayed inode pointers to
// space something else can take. Before the commit point it is the
// exact opposite: nothing landed, and not freeing them leaks. The two
// used to be one `return 0`, and every caller rolled back.
//
// jbd2 draws the same line, and answers it the same way
// (jbd2_journal_abort() plus errors=remount-ro): a filesystem that
// cannot apply its own commit stops writing, because the next mount's
// replay is the only thing that can still finish the job.
enum t3_commit {
    T3_COMMIT_ABORTED   = 0,   // nothing durable -- the caller must roll back
    T3_COMMIT_OK        = 1,
    T3_COMMIT_UNAPPLIED = 2,   // committed; replay owns these blocks now
};

// THE JOURNAL IMAGES IN AS FEW COMMANDS AS THE DEVICE TAKES. The
// journal's data blocks are contiguous on disk and txn_img is
// contiguous in memory, so a commit that wrote them one 4 KiB command at
// a time was paying per-command latency up to 32 times for one run.
static int write_journal_images(struct t3_state *sbi) {
    uint32_t per = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
    if (per == 0) per = 1;
    for (int i = 0; i < sbi->txn_count; ) {
        uint32_t k = (uint32_t)(sbi->txn_count - i);
        if (k > per) k = per;
        if (!t3_vol_write_sectors(sbi, (sbi->jdata_block + (uint32_t)i) * T3_SPB,
                               (int)(k * T3_SPB), sbi->txn_img[i])) return 0;
        i += (int)k;
    }
    return 1;
}

// THE TARGETS ALL IN FLIGHT AT ONCE. They are scattered, independent,
// and fenced by the barriers either side, so their order among
// themselves never mattered -- which is exactly a batch
// (blkdev_submit_batch(); AHCI queues it with NCQ, others loop). In the
// mount rather than on the stack, for the frame budget.

static int write_targets(struct t3_state *sbi) {
    if (sbi->readonly) return 0;             // t3_vol_write_sectors()'s gate
    t3_rcache_drop(sbi);                         // ...and its cache rule
    for (int i = 0; i < sbi->txn_count; i++) {
        uint32_t lba = sbi->txn_target[i] * T3_SPB;
        if (lba + T3_SPB > sbi->vol.sector_count) return 0;
        sbi->txn_io[i] = (struct blk_io){ .lba = sbi->vol.base_lba + lba, .count = T3_SPB,
                                       .write = 1, .buf = sbi->txn_img[i] };
    }
    return blkdev_submit_batch(sbi->vol.dev, sbi->txn_io, sbi->txn_count);
}

static enum t3_commit txn_commit_raw(struct t3_state *sbi) {
    if (sbi->txn_count == 0) return T3_COMMIT_OK;
    sbi->jrn_seq++;
    if (!write_journal_images(sbi)) { t3_txn_reset(sbi); return T3_COMMIT_ABORTED; }
    if (!write_journal_header(sbi, 1)) { t3_txn_reset(sbi); return T3_COMMIT_ABORTED; }

    // BARRIER 1, and it is checked. The journal's whole guarantee is
    // that everything written before this point is on the platter, so
    // if the flush cannot say that, the targets below must NOT be
    // overwritten: a half-written target with no durable journal behind
    // it is unrecoverable, while abandoning the transaction here costs
    // nothing that was not already lost. blkdev_flush(sbi->vol.dev) returned void until
    // a write-back cache went in underneath (ata_cache.h) -- that is
    // where a deferred write's failure now surfaces.
    if (!txn_barrier(sbi)) {
        klog_write(KLOG_ERR "tfs3: journal barrier failed -- transaction abandoned, "
                   "targets untouched\n");
        t3_txn_reset(sbi);
        return T3_COMMIT_ABORTED;
    }

    int ok = write_targets(sbi);

    // BARRIER 2: the targets must be durable before the commit flag is
    // cleared below, or a crash after clearing it loses the record of
    // work that never reached the disk. A failure here takes the same
    // path a failed target write does -- leave the header committed and
    // let replay finish the job next boot.
    if (!txn_barrier(sbi)) ok = 0;
    if (ok) {
        int saved = sbi->txn_count;
        sbi->txn_count = 0;
        write_journal_header(sbi, 0); // no barrier -- see the discipline note above
        sbi->txn_count = saved;
    } else {
        // Leave the header committed: replay finishes the job next
        // boot, same call t3_replay_journal() in TFS2 makes.
        // Either a target write or barrier 2 -- both mean the same
        // thing here, and only the journal can finish it now.
        klog_write(KLOG_ERR "tfs3: transaction could not be applied -- left committed for replay\n");
    }
    t3_txn_reset(sbi);
    return ok ? T3_COMMIT_OK : T3_COMMIT_UNAPPLIED;
}

// What every caller in this file actually wants: 1 on success, 0 on
// failure. The three-way answer is consumed HERE rather than at the
// sixteen call sites, which is what makes the rule impossible to
// forget -- the same argument t3_txn_begin() makes for forcing a deferred
// flush in one place. A caller's own t3_alog_rollback() then frees
// nothing, because there is nothing it may undo.
int t3_txn_commit(struct t3_state *sbi) {
    enum t3_commit r = txn_commit_raw(sbi);
    if (r == T3_COMMIT_UNAPPLIED) {
        t3_alog_cancel(sbi);
        t3_vol_go_readonly(sbi, "a committed transaction could not be applied");
    }
    return r == T3_COMMIT_OK;
}

// 1 if nothing is outstanding -- replayed, torn and discarded, or
// none to begin with. 0 when a COMMITTED transaction is still on the
// disk unapplied, which the caller must not mount writable over: a
// write could reuse a block the journal names, and the replay is the
// only thing that can still finish it.
int t3_replay_journal(struct t3_state *sbi) {
    uint8_t sec[T3_SECTOR];
    if (!t3_vol_read_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec)) return 1;
    if (!(sec[0] == 'J' && sec[1] == 'R' && sec[2] == 'N' && sec[3] == '3')) return 1;
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = t3_jh_cksum_off(sbi->sb.version);
    if (k_fnv1a(sec, ck) != rd32(sec + ck)) return 1; // torn header = no transaction
    sbi->jrn_seq = rd32(sec + 8);
    if (!sec[4]) return 1; // not committed
    uint32_t count = sec[5];
    if (count == 0 || count > sbi->jslots) count = 0;

    int all_ok = (count > 0);
    for (uint32_t i = 0; i < count && all_ok; i++) {
        if (!t3_read_block(sbi, sbi->jdata_block + i, sbi->txn_img[i])) all_ok = 0;
        else if (k_fnv1a(sbi->txn_img[i], T3_BLOCK) != rd32(sec + slots + i * 8 + 4)) all_ok = 0;
    }
    if (all_ok) {
        for (uint32_t i = 0; i < count && all_ok; i++) {
            if (!t3_write_block(sbi, rd32(sec + slots + i * 8), sbi->txn_img[i])) all_ok = 0;
        }
        if (all_ok && !blkdev_flush(sbi->vol.dev)) {
            // Same rule as t3_txn_commit()'s barrier 2: if the replayed
            // targets are not durable, do NOT clear the committed flag
            // below -- the next boot must replay them again.
            klog_write(KLOG_ERR "tfs3: journal replay barrier failed -- left committed "
                       "for next boot\n");
            return 0;
        }
        if (all_ok) {
            klog_write("tfs3: replayed a committed journal transaction (");
            klog_write_dec(count); klog_write(" blocks)\n");
        } else {
            klog_write(KLOG_ERR "tfs3: journal replay write failed -- left committed for next boot\n");
            return 0;
        }
    } else {
        klog_write("tfs3: discarded a torn journal transaction\n");
    }
    sbi->txn_count = 0;
    write_journal_header(sbi, 0);
    return 1;
}

// ---- inode staging (through the transaction) ------------------------------

static void pack_inode_into(uint8_t *p, const struct t3_inode *node) {
    k_memset(p, 0, T3_INODE_SIZE);
    p[0] = node->type;
    wr16(p + 2, node->links);
    wr64(p + 4, node->size);
    wr64(p + 12, node->created);
    wr64(p + 20, node->modified);
    for (int i = 0; i < 15; i++) wr32(p + 28 + i * 4, node->ptrs[i]);
    wr16(p + 92, node->mode);   // see struct t3_inode -- inside the checksum
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    wr32(p + 88, k_fnv1a(chk, sizeof(chk)));
}

// Patch inode `ino`'s 128 bytes inside its (journal-staged) table
// block. `node == 0` zeroes the slot -- a dead inode fails its
// checksum by design, the bitmap is the allocation authority.
int t3_txn_stage_inode(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node) {
    sbi->ino_gen++;   // see t3_state.ino_gen
    uint32_t lba, off;
    if (!t3_inode_pos(sbi, ino, &lba, &off)) return 0;
    uint32_t blk = lba / T3_SPB;
    uint32_t within = (lba % T3_SPB) * T3_SECTOR + off;
    uint8_t *img = t3_txn_stage(sbi, blk);
    if (!img) return 0;
    if (node) pack_inode_into(img + within, node);
    else k_memset(img + within, 0, T3_INODE_SIZE);
    return 1;
}

uint64_t t3_now_epoch(void) {
    struct rtc_time t;
    ktime_read(&t);
    return cal_rtc_to_epoch(&t);
}
