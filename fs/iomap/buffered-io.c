// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (C) 2016-2019 Christoph Hellwig.
 */
#include <linux/module.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/buffer_head.h>
#include <linux/dax.h>
#include <linux/writeback.h>
#include <linux/list_sort.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/sched/signal.h>
#include <linux/migrate.h>
#include "trace.h"

#include "../internal.h"

/*
 * Structure allocated for each page or THP when block size < page size
 * to track sub-page uptodate status and I/O completions.
 */
struct iomap_page {
	atomic_t		read_bytes_pending;
	atomic_t		write_bytes_pending;
	spinlock_t		uptodate_lock;
	unsigned long		uptodate[];
};

static inline struct iomap_page *to_iomap_page(struct folio *folio)
{
	if (folio_has_private(folio))
		return (struct iomap_page *)page_private(&folio->page);
	return NULL;
}

void dump_iomap_page(struct page *page, const char *reason)
{
	struct folio *folio = page_folio(page);
	struct iomap_page *iop = to_iomap_page(folio);
	unsigned int nr_blocks = i_blocks_per_folio(page->mapping->host, folio);

       dump_page(page, reason);
       if (iop)
               printk("iop:reads %d writes %d uptodate %*pb\n",
                               atomic_read(&iop->read_bytes_pending),
                               atomic_read(&iop->write_bytes_pending),
                               nr_blocks, iop->uptodate);
       else
               printk("iop:none\n");
}

static struct bio_set iomap_ioend_bioset;

static struct iomap_page *
iomap_page_create(struct inode *inode, struct folio *folio)
{
	struct iomap_page *iop = to_iomap_page(folio);
	unsigned int nr_blocks = i_blocks_per_folio(inode, folio);

	if (iop || nr_blocks <= 1)
		return iop;

	iop = kzalloc(struct_size(iop, uptodate, BITS_TO_LONGS(nr_blocks)),
			GFP_NOFS | __GFP_NOFAIL);
	spin_lock_init(&iop->uptodate_lock);
	if (FolioUptodate(folio))
		bitmap_fill(iop->uptodate, nr_blocks);
	attach_page_private(&folio->page, iop);
	return iop;
}

static void
iomap_page_release(struct page *page)
{
	struct folio *folio = page_folio(page);
	struct iomap_page *iop = detach_page_private(page);
	unsigned int nr_blocks = i_blocks_per_folio(page->mapping->host, folio);

	if (!iop)
		return;
	WARN_ON_ONCE(atomic_read(&iop->read_bytes_pending));
	WARN_ON_ONCE(atomic_read(&iop->write_bytes_pending));
	WARN_ON_ONCE(bitmap_full(iop->uptodate, nr_blocks) !=
			FolioUptodate(folio));
	kfree(iop);
}

/*
 * Calculate the range inside the page that we actually need to read.
 */
static void iomap_adjust_read_range(struct inode *inode, struct folio *folio,
		loff_t *pos, loff_t length, size_t *offp, size_t *lenp)
{
	struct iomap_page *iop = to_iomap_page(folio);
	loff_t orig_pos = *pos;
	loff_t isize = i_size_read(inode);
	unsigned block_bits = inode->i_blkbits;
	unsigned block_size = (1 << block_bits);
	size_t poff = offset_in_folio(folio, *pos);
	size_t plen = min_t(loff_t, folio_size(folio) - poff, length);
	unsigned first = poff >> block_bits;
	unsigned last = (poff + plen - 1) >> block_bits;

	/*
	 * If the block size is smaller than the page size we need to check the
	 * per-block uptodate status and adjust the offset and length if needed
	 * to avoid reading in already uptodate ranges.
	 */
	if (iop) {
		unsigned int i;

		/* move forward for each leading block marked uptodate */
		for (i = first; i <= last; i++) {
			if (!test_bit(i, iop->uptodate))
				break;
			*pos += block_size;
			poff += block_size;
			plen -= block_size;
			first++;
		}

		/* truncate len if we find any trailing uptodate block(s) */
		for ( ; i <= last; i++) {
			if (test_bit(i, iop->uptodate)) {
				plen -= (last - i + 1) * block_size;
				last = i - 1;
				break;
			}
		}
	}

	/*
	 * If the extent spans the block that contains the i_size we need to
	 * handle both halves separately so that we properly zero data in the
	 * page cache for blocks that are entirely outside of i_size.
	 */
	if (orig_pos <= isize && orig_pos + length > isize) {
		unsigned end = offset_in_folio(folio, isize - 1) >> block_bits;

		if (first <= end && last > end)
			plen -= (last - end) * block_size;
	}

	*offp = poff;
	*lenp = plen;
}

static void
iomap_iop_set_range_uptodate(struct folio *folio, size_t off, size_t len)
{
	struct iomap_page *iop = to_iomap_page(folio);
	struct inode *inode = folio->page.mapping->host;
	unsigned first = off >> inode->i_blkbits;
	unsigned last = (off + len - 1) >> inode->i_blkbits;
	unsigned long flags;

	spin_lock_irqsave(&iop->uptodate_lock, flags);
	bitmap_set(iop->uptodate, first, last - first + 1);
	if (bitmap_full(iop->uptodate, i_blocks_per_folio(inode, folio)))
		SetFolioUptodate(folio);
	spin_unlock_irqrestore(&iop->uptodate_lock, flags);
}

static void
iomap_set_range_uptodate(struct folio *folio, size_t off, size_t len)
{
	if (FolioError(folio))
		return;

	if (folio_has_private(folio))
		iomap_iop_set_range_uptodate(folio, off, len);
	else
		SetFolioUptodate(folio);
}

static void iomap_finish_folio_read(struct folio *folio, size_t offset,
		size_t len, int error)
{
	struct iomap_page *iop = to_iomap_page(folio);

	if (unlikely(error)) {
		ClearFolioUptodate(folio);
		SetFolioError(folio);
	} else {
		iomap_set_range_uptodate(folio, offset, len);
	}

	if (!iop || atomic_sub_and_test(len, &iop->read_bytes_pending))
		unlock_folio(folio);
}

static void iomap_read_end_io(struct bio *bio)
{
	int error = blk_status_to_errno(bio->bi_status);
	struct folio_iter fi;

	bio_for_each_folio_all(fi, bio)
		iomap_finish_folio_read(fi.folio, fi.offset, fi.length, error);
	bio_put(bio);
}

struct iomap_readpage_ctx {
	struct folio		*cur_folio;
	bool			cur_folio_in_bio;
	struct bio		*bio;
	struct readahead_control *rac;
};

static void iomap_read_inline_data(struct inode *inode, struct folio *folio,
		struct iomap *iomap)
{
	size_t size = i_size_read(inode);
	void *addr;

	if (FolioUptodate(folio))
		return;

	BUG_ON(folio->page.index);
	BUG_ON(FolioMulti(folio));
	BUG_ON(size > PAGE_SIZE - offset_in_page(iomap->inline_data));

	addr = kmap_local_page(&folio->page);
	memcpy(addr, iomap->inline_data, size);
	memset(addr + size, 0, PAGE_SIZE - size);
	kunmap_local(addr);
	SetFolioUptodate(folio);
}

static inline bool iomap_block_needs_zeroing(struct inode *inode,
		struct iomap *iomap, loff_t pos)
{
	return iomap->type != IOMAP_MAPPED ||
		(iomap->flags & IOMAP_F_NEW) ||
		pos >= i_size_read(inode);
}

static loff_t
iomap_readpage_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap, struct iomap *srcmap)
{
	struct iomap_readpage_ctx *ctx = data;
	struct folio *folio = ctx->cur_folio;
	struct iomap_page *iop = iomap_page_create(inode, folio);
	bool same_page = false, is_contig = false;
	loff_t orig_pos = pos;
	size_t poff, plen;
	sector_t sector;

	if (iomap->type == IOMAP_INLINE) {
		WARN_ON_ONCE(pos);
		iomap_read_inline_data(inode, folio, iomap);
		return PAGE_SIZE;
	}

	/* zero post-eof blocks as the page may be mapped */
	iomap_adjust_read_range(inode, folio, &pos, length, &poff, &plen);
	if (plen == 0)
		goto done;

	if (iomap_block_needs_zeroing(inode, iomap, pos)) {
		zero_user(&folio->page, poff, plen);
		iomap_set_range_uptodate(folio, poff, plen);
		goto done;
	}

	ctx->cur_folio_in_bio = true;
	if (iop)
		atomic_add(plen, &iop->read_bytes_pending);

	/* Try to merge into a previous segment if we can */
	sector = iomap_sector(iomap, pos);
	if (ctx->bio && bio_end_sector(ctx->bio) == sector) {
		if (__bio_try_merge_page(ctx->bio, &folio->page, plen, poff,
				&same_page))
			goto done;
		is_contig = true;
	}

	if (!is_contig || bio_full(ctx->bio, plen)) {
		gfp_t gfp = mapping_gfp_constraint(folio->page.mapping,
							GFP_KERNEL);
		gfp_t orig_gfp = gfp;
		int nr_vecs = (length + PAGE_SIZE - 1) >> PAGE_SHIFT;

		if (ctx->bio)
			submit_bio(ctx->bio);

		if (ctx->rac) /* same as readahead_gfp_mask */
			gfp |= __GFP_NORETRY | __GFP_NOWARN;
		ctx->bio = bio_alloc(gfp, min(BIO_MAX_PAGES, nr_vecs));
		/*
		 * If the bio_alloc fails, try it again for a single page to
		 * avoid having to deal with partial page reads.  This emulates
		 * what do_mpage_readpage does.
		 */
		if (!ctx->bio)
			ctx->bio = bio_alloc(orig_gfp, 1);
		ctx->bio->bi_opf = REQ_OP_READ;
		if (ctx->rac)
			ctx->bio->bi_opf |= REQ_RAHEAD;
		ctx->bio->bi_iter.bi_sector = sector;
		bio_set_dev(ctx->bio, iomap->bdev);
		ctx->bio->bi_end_io = iomap_read_end_io;
	}

	bio_add_page(ctx->bio, &folio->page, plen, poff);
done:
	/*
	 * Move the caller beyond our range so that it keeps making progress.
	 * For that we have to include any leading non-uptodate ranges, but
	 * we can skip trailing ones as they will be handled in the next
	 * iteration.
	 */
	return pos - orig_pos + plen;
}

int iomap_readpage(struct folio *folio, const struct iomap_ops *ops)
{
	struct iomap_readpage_ctx ctx = { .cur_folio = folio };
	struct inode *inode = folio->page.mapping->host;
	size_t poff;
	loff_t ret;

	trace_iomap_readpage(inode, 1);

	for (poff = 0; poff < folio_size(folio); poff += ret) {
		ret = iomap_apply(inode, folio_offset(folio) + poff,
				folio_size(folio) - poff, 0, ops, &ctx,
				iomap_readpage_actor);
		if (ret <= 0) {
			WARN_ON_ONCE(ret == 0);
			SetFolioError(folio);
			break;
		}
	}

	if (ctx.bio) {
		submit_bio(ctx.bio);
		WARN_ON_ONCE(!ctx.cur_folio_in_bio);
	} else {
		WARN_ON_ONCE(ctx.cur_folio_in_bio);
		unlock_folio(folio);
	}

	/*
	 * Just like mpage_readahead and block_read_full_page we always
	 * return 0 and just mark the page as PageError on errors.  This
	 * should be cleaned up all through the stack eventually.
	 */
	return 0;
}
EXPORT_SYMBOL_GPL(iomap_readpage);

static loff_t
iomap_readahead_actor(struct inode *inode, loff_t pos, loff_t length,
		void *data, struct iomap *iomap, struct iomap *srcmap)
{
	struct iomap_readpage_ctx *ctx = data;
	loff_t done, ret;

	for (done = 0; done < length; done += ret) {
		if (ctx->cur_folio &&
		    offset_in_folio(ctx->cur_folio, pos + done) == 0) {
			if (!ctx->cur_folio_in_bio)
				unlock_folio(ctx->cur_folio);
			ctx->cur_folio = NULL;
		}
		if (!ctx->cur_folio) {
			ctx->cur_folio = readahead_folio(ctx->rac);
			ctx->cur_folio_in_bio = false;
		}
		ret = iomap_readpage_actor(inode, pos + done, length - done,
				ctx, iomap, srcmap);
	}

	return done;
}

/**
 * iomap_readahead - Attempt to read pages from a file.
 * @rac: Describes the pages to be read.
 * @ops: The operations vector for the filesystem.
 *
 * This function is for filesystems to call to implement their readahead
 * address_space operation.
 *
 * Context: The @ops callbacks may submit I/O (eg to read the addresses of
 * blocks from disc), and may wait for it.  The caller may be trying to
 * access a different page, and so sleeping excessively should be avoided.
 * It may allocate memory, but should avoid costly allocations.  This
 * function is called with memalloc_nofs set, so allocations will not cause
 * the filesystem to be reentered.
 */
void iomap_readahead(struct readahead_control *rac, const struct iomap_ops *ops)
{
	struct inode *inode = rac->mapping->host;
	loff_t pos = readahead_pos(rac);
	loff_t length = readahead_length(rac);
	struct iomap_readpage_ctx ctx = {
		.rac	= rac,
	};

	trace_iomap_readahead(inode, readahead_count(rac));

	while (length > 0) {
		loff_t ret = iomap_apply(inode, pos, length, 0, ops,
				&ctx, iomap_readahead_actor);
		if (ret <= 0) {
			WARN_ON_ONCE(ret == 0);
			break;
		}
		pos += ret;
		length -= ret;
	}

	if (ctx.bio)
		submit_bio(ctx.bio);
	if (ctx.cur_folio) {
		if (!ctx.cur_folio_in_bio)
			unlock_folio(ctx.cur_folio);
	}
}
EXPORT_SYMBOL_GPL(iomap_readahead);

/*
 * iomap_is_partially_uptodate checks whether blocks within a page are
 * uptodate or not.
 *
 * Returns true if all blocks which correspond to a file portion
 * we want to read within the page are uptodate.
 */
bool iomap_is_partially_uptodate(struct folio *folio, size_t from, size_t count)
{
	struct iomap_page *iop = to_iomap_page(folio);
	struct inode *inode = folio->page.mapping->host;
	size_t len;
	unsigned first, last, i;

	if (!iop)
		return false;

	/* Limit range to this folio */
	len = min(folio_size(folio) - from, count);

	/* First and last blocks in range within page */
	first = from >> inode->i_blkbits;
	last = (from + len - 1) >> inode->i_blkbits;

	for (i = first; i <= last; i++)
		if (!test_bit(i, iop->uptodate))
			return false;
	return true;
}
EXPORT_SYMBOL_GPL(iomap_is_partially_uptodate);

int
iomap_releasepage(struct page *page, gfp_t gfp_mask)
{
	trace_iomap_releasepage(page->mapping->host, page_offset(page),
			PAGE_SIZE);

	/*
	 * mm accommodates an old ext3 case where clean pages might not have had
	 * the dirty bit cleared. Thus, it can send actual dirty pages to
	 * ->releasepage() via shrink_active_list(), skip those here.
	 */
	if (PageDirty(page) || PageWriteback(page))
		return 0;
	iomap_page_release(page);
	return 1;
}
EXPORT_SYMBOL_GPL(iomap_releasepage);

void
iomap_invalidatepage(struct page *page, unsigned int offset, unsigned int len)
{
	trace_iomap_invalidatepage(page->mapping->host, offset, len);

	/*
	 * If we are invalidating the entire page, clear the dirty state from it
	 * and release it to avoid unnecessary buildup of the LRU.
	 */
	if (offset == 0 && len == PAGE_SIZE) {
		WARN_ON_ONCE(PageWriteback(page));
		cancel_dirty_page(page);
		iomap_page_release(page);
	}
}
EXPORT_SYMBOL_GPL(iomap_invalidatepage);

#ifdef CONFIG_MIGRATION
int
iomap_migrate_page(struct address_space *mapping, struct page *newpage,
		struct page *page, enum migrate_mode mode)
{
	int ret;

	ret = migrate_page_move_mapping(mapping, newpage, page, 0);
	if (ret != MIGRATEPAGE_SUCCESS)
		return ret;

	if (page_has_private(page))
		attach_page_private(newpage, detach_page_private(page));

	if (mode != MIGRATE_SYNC_NO_COPY)
		migrate_page_copy(newpage, page);
	else
		migrate_page_states(newpage, page);
	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL_GPL(iomap_migrate_page);
#endif /* CONFIG_MIGRATION */

enum {
	IOMAP_WRITE_F_UNSHARE		= (1 << 0),
};

static void
iomap_write_failed(struct inode *inode, loff_t pos, unsigned len)
{
	loff_t i_size = i_size_read(inode);

	/*
	 * Only truncate newly allocated pages beyoned EOF, even if the
	 * write started inside the existing inode size.
	 */
	if (pos + len > i_size)
		truncate_pagecache_range(inode, max(pos, i_size), pos + len);
}

static int iomap_read_folio_sync(loff_t block_start, struct folio *folio,
		size_t poff, size_t plen, struct iomap *iomap)
{
	struct bio_vec bvec;
	struct bio bio;

	bio_init(&bio, &bvec, 1);
	bio.bi_opf = REQ_OP_READ;
	bio.bi_iter.bi_sector = iomap_sector(iomap, block_start);
	bio_set_dev(&bio, iomap->bdev);
	__bio_add_page(&bio, &folio->page, plen, poff);
	return submit_bio_wait(&bio);
}

static int
__iomap_write_begin(struct inode *inode, loff_t pos, unsigned len, int flags,
		struct folio *folio, struct iomap *srcmap)
{
	loff_t block_size = i_blocksize(inode);
	loff_t block_start = round_down(pos, block_size);
	loff_t block_end = round_up(pos + len, block_size);
	size_t from = offset_in_folio(folio, pos), to = from + len;
	size_t poff, plen;

	iomap_page_create(inode, folio);
	if (FolioUptodate(folio))
		return 0;
	ClearFolioError(folio);

	do {
		iomap_adjust_read_range(inode, folio, &block_start,
				block_end - block_start, &poff, &plen);
		if (plen == 0)
			break;

		if (!(flags & IOMAP_WRITE_F_UNSHARE) &&
		    (from <= poff || from >= poff + plen) &&
		    (to <= poff || to >= poff + plen))
			continue;

		if (iomap_block_needs_zeroing(inode, srcmap, block_start)) {
			if (WARN_ON_ONCE(flags & IOMAP_WRITE_F_UNSHARE))
				return -EIO;
			zero_user_segments(&folio->page, poff, from, to,
					poff + plen);
		} else {
			int status = iomap_read_folio_sync(block_start,
					folio, poff, plen, srcmap);
			if (status)
				return status;
		}
		iomap_set_range_uptodate(folio, poff, plen);
	} while ((block_start += plen) < block_end);

	return 0;
}

static int iomap_write_begin(struct inode *inode, loff_t pos, unsigned len,
		unsigned flags, struct folio **foliop, struct iomap *iomap,
		struct iomap *srcmap)
{
	const struct iomap_page_ops *page_ops = iomap->page_ops;
	struct folio *folio;
	int status = 0;

	BUG_ON(pos + len > iomap->offset + iomap->length);
	if (srcmap != iomap)
		BUG_ON(pos + len > srcmap->offset + srcmap->length);

	if (fatal_signal_pending(current))
		return -EINTR;

	if (page_ops && page_ops->page_prepare) {
		status = page_ops->page_prepare(inode, pos, len, iomap);
		if (status)
			return status;
	}

	folio = filemap_get_stable_folio(inode->i_mapping, pos >> PAGE_SHIFT,
			AOP_FLAG_NOFS);
	if (!folio) {
		status = -ENOMEM;
		goto out_no_folio;
	}

	if (srcmap->type == IOMAP_INLINE)
		iomap_read_inline_data(inode, folio, srcmap);
	else if (iomap->flags & IOMAP_F_BUFFER_HEAD)
		status = __block_write_begin_int(&folio->page, pos, len, NULL,
				srcmap);
	else
		status = __iomap_write_begin(inode, pos, len, flags, folio,
				srcmap);

	if (unlikely(status))
		goto out_unlock;

	*foliop = folio;
	return 0;

out_unlock:
	unlock_folio(folio);
	put_folio(folio);
	iomap_write_failed(inode, pos, len);

out_no_folio:
	if (page_ops && page_ops->page_done)
		page_ops->page_done(inode, pos, 0, NULL, iomap);
	return status;
}

static size_t __iomap_write_end(struct inode *inode, loff_t pos, size_t len,
		size_t copied, struct folio *folio)
{
	flush_dcache_folio(folio);

	/*
	 * The blocks that were entirely written will now be uptodate, so we
	 * don't have to worry about a readpage reading them and overwriting a
	 * partial write.  However if we have encountered a short write and only
	 * partially written into a block, it will not be marked uptodate, so a
	 * readpage might come in and destroy our partial write.
	 *
	 * Do the simplest thing, and just treat any short write to a non
	 * uptodate page as a zero-length write, and force the caller to redo
	 * the whole thing.
	 */
	if (unlikely(copied < len && !FolioUptodate(folio)))
		return 0;
	iomap_set_range_uptodate(folio, offset_in_folio(folio, pos), len);
	iomap_set_page_dirty(inode->i_mapping, folio);
	return copied;
}

static size_t iomap_write_end_inline(struct inode *inode, struct page *page,
		struct iomap *iomap, loff_t pos, size_t copied)
{
	void *addr;

	WARN_ON_ONCE(!PageUptodate(page));
	BUG_ON(pos + copied > PAGE_SIZE - offset_in_page(iomap->inline_data));

	flush_dcache_page(page);
	addr = kmap_atomic(page);
	memcpy(iomap->inline_data + pos, addr + pos, copied);
	kunmap_atomic(addr);

	mark_inode_dirty(inode);
	return copied;
}

/* Returns the number of bytes copied.  May be 0.  Cannot be an errno. */
static size_t iomap_write_end(struct inode *inode, loff_t pos, size_t len,
		size_t copied, struct folio *folio, struct iomap *iomap,
		struct iomap *srcmap)
{
	const struct iomap_page_ops *page_ops = iomap->page_ops;
	loff_t old_size = inode->i_size;
	size_t ret;

	if (srcmap->type == IOMAP_INLINE) {
		ret = iomap_write_end_inline(inode, &folio->page, iomap, pos,
				copied);
	} else if (srcmap->flags & IOMAP_F_BUFFER_HEAD) {
		ret = block_write_end(NULL, inode->i_mapping, pos, len, copied,
				&folio->page, NULL);
	} else {
		ret = __iomap_write_end(inode, pos, len, copied, folio);
	}

	/*
	 * Update the in-memory inode size after copying the data into the page
	 * cache.  It's up to the file system to write the updated size to disk,
	 * preferably after I/O completion so that no stale data is exposed.
	 */
	if (pos + ret > old_size) {
		i_size_write(inode, pos + ret);
		iomap->flags |= IOMAP_F_SIZE_CHANGED;
	}
	unlock_folio(folio);

	if (old_size < pos)
		pagecache_isize_extended(inode, old_size, pos);
	if (page_ops && page_ops->page_done)
		page_ops->page_done(inode, pos, ret, &folio->page, iomap);
	put_folio(folio);

	if (ret < len)
		iomap_write_failed(inode, pos, len);
	return ret;
}

static loff_t
iomap_write_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap, struct iomap *srcmap)
{
	struct iov_iter *i = data;
	long status = 0;
	ssize_t written = 0;

	do {
		struct folio *folio;
		unsigned long offset;	/* Offset into pagecache page */
		size_t bytes;		/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */

		offset = offset_in_page(pos);
		bytes = min_t(size_t, PAGE_SIZE - offset, iov_iter_count(i));
again:
		if (bytes > length)
			bytes = length;

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 *
		 * Not only is this an optimisation, but it is also required
		 * to check that the address is actually valid, when atomic
		 * usercopies are used, below.
		 */
		if (unlikely(iov_iter_fault_in_readable(i, bytes))) {
			status = -EFAULT;
			break;
		}

		status = iomap_write_begin(inode, pos, bytes, 0, &folio, iomap,
				srcmap);
		if (unlikely(status))
			break;

		if (mapping_writably_mapped(inode->i_mapping))
			flush_dcache_folio(folio);

		/* We may be part-way through a folio */
		offset = offset_in_folio(folio, pos);
		copied = iov_iter_copy_from_user_atomic(folio, i, offset,
				bytes);

		copied = iomap_write_end(inode, pos, bytes, copied, folio,
				iomap, srcmap);

		cond_resched();

		iov_iter_advance(i, copied);
		if (unlikely(copied == 0)) {
			/*
			 * If we were unable to copy any data at all, we must
			 * fall back to a single segment length write.
			 *
			 * If we didn't fallback here, we could livelock
			 * because not all segments in the iov can be copied at
			 * once without a pagefault.
			 */
			bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_single_seg_count(i));
			goto again;
		}
		pos += copied;
		written += copied;
		length -= copied;

		balance_dirty_pages_ratelimited(inode->i_mapping);
	} while (iov_iter_count(i) && length);

	return written ? written : status;
}

ssize_t
iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *iter,
		const struct iomap_ops *ops)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	loff_t pos = iocb->ki_pos, ret = 0, written = 0;

	while (iov_iter_count(iter)) {
		ret = iomap_apply(inode, pos, iov_iter_count(iter),
				IOMAP_WRITE, ops, iter, iomap_write_actor);
		if (ret <= 0)
			break;
		pos += ret;
		written += ret;
	}

	return written ? written : ret;
}
EXPORT_SYMBOL_GPL(iomap_file_buffered_write);

static loff_t
iomap_unshare_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap, struct iomap *srcmap)
{
	long status = 0;
	loff_t written = 0;

	/* don't bother with blocks that are not shared to start with */
	if (!(iomap->flags & IOMAP_F_SHARED))
		return length;
	/* don't bother with holes or unwritten extents */
	if (srcmap->type == IOMAP_HOLE || srcmap->type == IOMAP_UNWRITTEN)
		return length;

	do {
		unsigned long offset = offset_in_page(pos);
		unsigned long bytes = min_t(loff_t, PAGE_SIZE - offset, length);
		struct folio *folio;

		status = iomap_write_begin(inode, pos, bytes,
				IOMAP_WRITE_F_UNSHARE, &folio, iomap, srcmap);
		if (unlikely(status))
			return status;

		status = iomap_write_end(inode, pos, bytes, bytes, folio, iomap,
				srcmap);
		if (WARN_ON_ONCE(status == 0))
			return -EIO;

		cond_resched();

		pos += status;
		written += status;
		length -= status;

		balance_dirty_pages_ratelimited(inode->i_mapping);
	} while (length);

	return written;
}

int
iomap_file_unshare(struct inode *inode, loff_t pos, loff_t len,
		const struct iomap_ops *ops)
{
	loff_t ret;

	while (len) {
		ret = iomap_apply(inode, pos, len, IOMAP_WRITE, ops, NULL,
				iomap_unshare_actor);
		if (ret <= 0)
			return ret;
		pos += ret;
		len -= ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iomap_file_unshare);

static s64 iomap_zero(struct inode *inode, loff_t pos, u64 length,
		struct iomap *iomap, struct iomap *srcmap)
{
	struct folio *folio;
	int status;
	unsigned offset = offset_in_page(pos);
	unsigned bytes = min_t(u64, PAGE_SIZE - offset, length);

	status = iomap_write_begin(inode, pos, bytes, 0, &folio, iomap, srcmap);
	if (status)
		return status;

	zero_user(&folio->page, offset, bytes);
	mark_folio_accessed(folio);

	return iomap_write_end(inode, pos, bytes, bytes, folio, iomap, srcmap);
}

static loff_t iomap_zero_range_actor(struct inode *inode, loff_t pos,
		loff_t length, void *data, struct iomap *iomap,
		struct iomap *srcmap)
{
	bool *did_zero = data;
	loff_t written = 0;

	/* already zeroed?  we're done. */
	if (srcmap->type == IOMAP_HOLE || srcmap->type == IOMAP_UNWRITTEN)
		return length;

	do {
		s64 bytes;

		if (IS_DAX(inode))
			bytes = dax_iomap_zero(pos, length, iomap);
		else
			bytes = iomap_zero(inode, pos, length, iomap, srcmap);
		if (bytes < 0)
			return bytes;

		pos += bytes;
		length -= bytes;
		written += bytes;
		if (did_zero)
			*did_zero = true;
	} while (length > 0);

	return written;
}

int
iomap_zero_range(struct inode *inode, loff_t pos, loff_t len, bool *did_zero,
		const struct iomap_ops *ops)
{
	loff_t ret;

	while (len > 0) {
		ret = iomap_apply(inode, pos, len, IOMAP_ZERO,
				ops, did_zero, iomap_zero_range_actor);
		if (ret <= 0)
			return ret;

		pos += ret;
		len -= ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iomap_zero_range);

int
iomap_truncate_page(struct inode *inode, loff_t pos, bool *did_zero,
		const struct iomap_ops *ops)
{
	unsigned int blocksize = i_blocksize(inode);
	unsigned int off = pos & (blocksize - 1);

	/* Block boundary? Nothing to do */
	if (!off)
		return 0;
	return iomap_zero_range(inode, pos, blocksize - off, did_zero, ops);
}
EXPORT_SYMBOL_GPL(iomap_truncate_page);

static loff_t
iomap_page_mkwrite_actor(struct inode *inode, loff_t pos, loff_t length,
		void *data, struct iomap *iomap, struct iomap *srcmap)
{
	struct folio *folio = data;
	int ret;

	if (iomap->flags & IOMAP_F_BUFFER_HEAD) {
		ret = __block_write_begin_int(&folio->page, pos, length, NULL,
				iomap);
		if (ret)
			return ret;
		block_commit_write(&folio->page, 0, length);
	} else {
		WARN_ON_ONCE(!FolioUptodate(folio));
		iomap_page_create(inode, folio);
		set_folio_dirty(folio);
	}

	return length;
}

vm_fault_t iomap_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops)
{
	struct folio *folio = page_folio(vmf->page);
	struct inode *inode = file_inode(vmf->vma->vm_file);
	unsigned long length;
	loff_t pos;
	ssize_t ret;

	lock_folio(folio);
	ret = folio_mkwrite_check_truncate(folio, inode);
	if (ret < 0)
		goto out_unlock;
	length = ret;

	pos = folio_offset(folio);
	while (length > 0) {
		ret = iomap_apply(inode, pos, length,
				IOMAP_WRITE | IOMAP_FAULT, ops, folio,
				iomap_page_mkwrite_actor);
		if (unlikely(ret <= 0))
			goto out_unlock;
		pos += ret;
		length -= ret;
	}

	wait_for_stable_folio(folio);
	return VM_FAULT_LOCKED;
out_unlock:
	unlock_folio(folio);
	return block_page_mkwrite_return(ret);
}
EXPORT_SYMBOL_GPL(iomap_page_mkwrite);

static void iomap_finish_folio_write(struct inode *inode, struct folio *folio,
		size_t len, int error)
{
	struct iomap_page *iop = to_iomap_page(folio);

	if (error) {
		SetFolioError(folio);
		mapping_set_error(inode->i_mapping, -EIO);
	}

	WARN_ON_ONCE(i_blocks_per_folio(inode, folio) > 1 && !iop);
	WARN_ON_ONCE(iop && atomic_read(&iop->write_bytes_pending) <= 0);

	if (!iop || atomic_sub_and_test(len, &iop->write_bytes_pending))
		end_folio_writeback(folio);
}

/*
 * We're now finished for good with this ioend structure.  Update the page
 * state, release holds on bios, and finally free up memory.  Do not use the
 * ioend after this.
 */
static void
iomap_finish_ioend(struct iomap_ioend *ioend, int error)
{
	struct inode *inode = ioend->io_inode;
	struct bio *bio = &ioend->io_inline_bio;
	struct bio *last = ioend->io_bio, *next;
	u64 start = bio->bi_iter.bi_sector;
	loff_t offset = ioend->io_offset;
	bool quiet = bio_flagged(bio, BIO_QUIET);

	for (bio = &ioend->io_inline_bio; bio; bio = next) {
		struct folio_iter fi;

		/*
		 * For the last bio, bi_private points to the ioend, so we
		 * need to explicitly end the iteration here.
		 */
		if (bio == last)
			next = NULL;
		else
			next = bio->bi_private;

		/* walk all folios in bio, ending page IO on them */
		bio_for_each_folio_all(fi, bio)
			iomap_finish_folio_write(inode, fi.folio, fi.length,
					error);
		bio_put(bio);
	}
	/* The ioend has been freed by bio_put() */

	if (unlikely(error && !quiet)) {
		printk_ratelimited(KERN_ERR
"%s: writeback error on inode %lu, offset %lld, sector %llu",
			inode->i_sb->s_id, inode->i_ino, offset, start);
	}
}

void
iomap_finish_ioends(struct iomap_ioend *ioend, int error)
{
	struct list_head tmp;

	list_replace_init(&ioend->io_list, &tmp);
	iomap_finish_ioend(ioend, error);

	while (!list_empty(&tmp)) {
		ioend = list_first_entry(&tmp, struct iomap_ioend, io_list);
		list_del_init(&ioend->io_list);
		iomap_finish_ioend(ioend, error);
	}
}
EXPORT_SYMBOL_GPL(iomap_finish_ioends);

/*
 * We can merge two adjacent ioends if they have the same set of work to do.
 */
static bool
iomap_ioend_can_merge(struct iomap_ioend *ioend, struct iomap_ioend *next)
{
	if (ioend->io_bio->bi_status != next->io_bio->bi_status)
		return false;
	if ((ioend->io_flags & IOMAP_F_SHARED) ^
	    (next->io_flags & IOMAP_F_SHARED))
		return false;
	if ((ioend->io_type == IOMAP_UNWRITTEN) ^
	    (next->io_type == IOMAP_UNWRITTEN))
		return false;
	if (ioend->io_offset + ioend->io_size != next->io_offset)
		return false;
	return true;
}

void
iomap_ioend_try_merge(struct iomap_ioend *ioend, struct list_head *more_ioends,
		void (*merge_private)(struct iomap_ioend *ioend,
				struct iomap_ioend *next))
{
	struct iomap_ioend *next;

	INIT_LIST_HEAD(&ioend->io_list);

	while ((next = list_first_entry_or_null(more_ioends, struct iomap_ioend,
			io_list))) {
		if (!iomap_ioend_can_merge(ioend, next))
			break;
		list_move_tail(&next->io_list, &ioend->io_list);
		ioend->io_size += next->io_size;
		if (next->io_private && merge_private)
			merge_private(ioend, next);
	}
}
EXPORT_SYMBOL_GPL(iomap_ioend_try_merge);

static int
iomap_ioend_compare(void *priv, struct list_head *a, struct list_head *b)
{
	struct iomap_ioend *ia = container_of(a, struct iomap_ioend, io_list);
	struct iomap_ioend *ib = container_of(b, struct iomap_ioend, io_list);

	if (ia->io_offset < ib->io_offset)
		return -1;
	if (ia->io_offset > ib->io_offset)
		return 1;
	return 0;
}

void
iomap_sort_ioends(struct list_head *ioend_list)
{
	list_sort(NULL, ioend_list, iomap_ioend_compare);
}
EXPORT_SYMBOL_GPL(iomap_sort_ioends);

static void iomap_writepage_end_bio(struct bio *bio)
{
	struct iomap_ioend *ioend = bio->bi_private;

	iomap_finish_ioend(ioend, blk_status_to_errno(bio->bi_status));
}

/*
 * Submit the final bio for an ioend.
 *
 * If @error is non-zero, it means that we have a situation where some part of
 * the submission process has failed after we have marked paged for writeback
 * and unlocked them.  In this situation, we need to fail the bio instead of
 * submitting it.  This typically only happens on a filesystem shutdown.
 */
static int
iomap_submit_ioend(struct iomap_writepage_ctx *wpc, struct iomap_ioend *ioend,
		int error)
{
	ioend->io_bio->bi_private = ioend;
	ioend->io_bio->bi_end_io = iomap_writepage_end_bio;

	if (wpc->ops->prepare_ioend)
		error = wpc->ops->prepare_ioend(ioend, error);
	if (error) {
		/*
		 * If we are failing the IO now, just mark the ioend with an
		 * error and finish it.  This will run IO completion immediately
		 * as there is only one reference to the ioend at this point in
		 * time.
		 */
		ioend->io_bio->bi_status = errno_to_blk_status(error);
		bio_endio(ioend->io_bio);
		return error;
	}

	submit_bio(ioend->io_bio);
	return 0;
}

static struct iomap_ioend *
iomap_alloc_ioend(struct inode *inode, struct iomap_writepage_ctx *wpc,
		loff_t offset, sector_t sector, struct writeback_control *wbc)
{
	struct iomap_ioend *ioend;
	struct bio *bio;

	bio = bio_alloc_bioset(GFP_NOFS, BIO_MAX_PAGES, &iomap_ioend_bioset);
	bio_set_dev(bio, wpc->iomap.bdev);
	bio->bi_iter.bi_sector = sector;
	bio->bi_opf = REQ_OP_WRITE | wbc_to_write_flags(wbc);
	bio->bi_write_hint = inode->i_write_hint;
	wbc_init_bio(wbc, bio);

	ioend = container_of(bio, struct iomap_ioend, io_inline_bio);
	INIT_LIST_HEAD(&ioend->io_list);
	ioend->io_type = wpc->iomap.type;
	ioend->io_flags = wpc->iomap.flags;
	ioend->io_inode = inode;
	ioend->io_size = 0;
	ioend->io_offset = offset;
	ioend->io_private = NULL;
	ioend->io_bio = bio;
	return ioend;
}

/*
 * Allocate a new bio, and chain the old bio to the new one.
 *
 * Note that we have to do perform the chaining in this unintuitive order
 * so that the bi_private linkage is set up in the right direction for the
 * traversal in iomap_finish_ioend().
 */
static struct bio *
iomap_chain_bio(struct bio *prev)
{
	struct bio *new;

	new = bio_alloc(GFP_NOFS, BIO_MAX_PAGES);
	bio_copy_dev(new, prev);/* also copies over blkcg information */
	new->bi_iter.bi_sector = bio_end_sector(prev);
	new->bi_opf = prev->bi_opf;
	new->bi_write_hint = prev->bi_write_hint;

	bio_chain(prev, new);
	bio_get(prev);		/* for iomap_finish_ioend */
	submit_bio(prev);
	return new;
}

static bool
iomap_can_add_to_ioend(struct iomap_writepage_ctx *wpc, loff_t offset,
		sector_t sector)
{
	if ((wpc->iomap.flags & IOMAP_F_SHARED) !=
	    (wpc->ioend->io_flags & IOMAP_F_SHARED))
		return false;
	if (wpc->iomap.type != wpc->ioend->io_type)
		return false;
	if (offset != wpc->ioend->io_offset + wpc->ioend->io_size)
		return false;
	if (sector != bio_end_sector(wpc->ioend->io_bio))
		return false;
	return true;
}

/*
 * Test to see if we have an existing ioend structure that we could append to
 * first, otherwise finish off the current ioend and start another.
 */
static void
iomap_add_to_ioend(struct inode *inode, loff_t offset, struct page *page,
		struct iomap_page *iop, struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct list_head *iolist)
{
	sector_t sector = iomap_sector(&wpc->iomap, offset);
	unsigned len = i_blocksize(inode);
	unsigned poff = offset & (PAGE_SIZE - 1);
	bool merged, same_page = false;

	if (!wpc->ioend || !iomap_can_add_to_ioend(wpc, offset, sector)) {
		if (wpc->ioend)
			list_add(&wpc->ioend->io_list, iolist);
		wpc->ioend = iomap_alloc_ioend(inode, wpc, offset, sector, wbc);
	}

	merged = __bio_try_merge_page(wpc->ioend->io_bio, page, len, poff,
			&same_page);
	if (iop)
		atomic_add(len, &iop->write_bytes_pending);

	if (!merged) {
		if (bio_full(wpc->ioend->io_bio, len)) {
			wpc->ioend->io_bio =
				iomap_chain_bio(wpc->ioend->io_bio);
		}
		bio_add_page(wpc->ioend->io_bio, page, len, poff);
	}

	wpc->ioend->io_size += len;
	wbc_account_cgroup_owner(wbc, page, len);
}

/*
 * We implement an immediate ioend submission policy here to avoid needing to
 * chain multiple ioends and hence nest mempool allocations which can violate
 * forward progress guarantees we need to provide. The current ioend we are
 * adding blocks to is cached on the writepage context, and if the new block
 * does not append to the cached ioend it will create a new ioend and cache that
 * instead.
 *
 * If a new ioend is created and cached, the old ioend is returned and queued
 * locally for submission once the entire page is processed or an error has been
 * detected.  While ioends are submitted immediately after they are completed,
 * batching optimisations are provided by higher level block plugging.
 *
 * At the end of a writeback pass, there will be a cached ioend remaining on the
 * writepage context that the caller will need to submit.
 */
static int
iomap_writepage_map(struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct inode *inode,
		struct page *page, u64 end_offset)
{
	struct folio *folio = page_folio(page);
	struct iomap_page *iop = to_iomap_page(folio);
	struct iomap_ioend *ioend, *next;
	unsigned len = i_blocksize(inode);
	u64 file_offset; /* file offset of page */
	int error = 0, count = 0, i;
	LIST_HEAD(submit_list);

	WARN_ON_ONCE(i_blocks_per_folio(inode, folio) > 1 && !iop);
	WARN_ON_ONCE(iop && atomic_read(&iop->write_bytes_pending) != 0);

	/*
	 * Walk through the page to find areas to write back. If we run off the
	 * end of the current map or find the current map invalid, grab a new
	 * one.
	 */
	for (i = 0, file_offset = page_offset(page);
	     i < (PAGE_SIZE >> inode->i_blkbits) && file_offset < end_offset;
	     i++, file_offset += len) {
		if (iop && !test_bit(i, iop->uptodate))
			continue;

		error = wpc->ops->map_blocks(wpc, inode, file_offset);
		if (error)
			break;
		if (WARN_ON_ONCE(wpc->iomap.type == IOMAP_INLINE))
			continue;
		if (wpc->iomap.type == IOMAP_HOLE)
			continue;
		iomap_add_to_ioend(inode, file_offset, page, iop, wpc, wbc,
				 &submit_list);
		count++;
	}

	WARN_ON_ONCE(!wpc->ioend && !list_empty(&submit_list));
	WARN_ON_ONCE(!FolioLocked(folio));
	WARN_ON_ONCE(FolioWriteback(folio));
	WARN_ON_ONCE(FolioDirty(folio));

	/*
	 * We cannot cancel the ioend directly here on error.  We may have
	 * already set other pages under writeback and hence we have to run I/O
	 * completion to mark the error state of the pages under writeback
	 * appropriately.
	 */
	if (unlikely(error)) {
		/*
		 * Let the filesystem know what portion of the current page
		 * failed to map. If the page wasn't been added to ioend, it
		 * won't be affected by I/O completion and we must unlock it
		 * now.
		 */
		if (wpc->ops->discard_page)
			wpc->ops->discard_page(page, file_offset);
		if (!count) {
			ClearFolioUptodate(folio);
			unlock_folio(folio);
			goto done;
		}
	}

	set_page_writeback(page);
	unlock_folio(folio);

	/*
	 * Preserve the original error if there was one, otherwise catch
	 * submission errors here and propagate into subsequent ioend
	 * submissions.
	 */
	list_for_each_entry_safe(ioend, next, &submit_list, io_list) {
		int error2;

		list_del_init(&ioend->io_list);
		error2 = iomap_submit_ioend(wpc, ioend, error);
		if (error2 && !error)
			error = error2;
	}

	/*
	 * We can end up here with no error and nothing to write only if we race
	 * with a partial page truncate on a sub-page block sized filesystem.
	 */
	if (!count)
		end_page_writeback(page);
done:
	mapping_set_error(page->mapping, error);
	return error;
}

/*
 * Write out a dirty page.
 *
 * For delalloc space on the page we need to allocate space and flush it.
 * For unwritten space on the page we need to start the conversion to
 * regular allocated space.
 */
static int
iomap_do_writepage(struct page *page, struct writeback_control *wbc, void *data)
{
	struct iomap_writepage_ctx *wpc = data;
	struct inode *inode = page->mapping->host;
	pgoff_t end_index;
	u64 end_offset;
	loff_t offset;

	trace_iomap_writepage(inode, page_offset(page), PAGE_SIZE);

	/*
	 * Refuse to write the page out if we are called from reclaim context.
	 *
	 * This avoids stack overflows when called from deeply used stacks in
	 * random callers for direct reclaim or memcg reclaim.  We explicitly
	 * allow reclaim from kswapd as the stack usage there is relatively low.
	 *
	 * This should never happen except in the case of a VM regression so
	 * warn about it.
	 */
	if (WARN_ON_ONCE((current->flags & (PF_MEMALLOC|PF_KSWAPD)) ==
			PF_MEMALLOC))
		goto redirty;

	/*
	 * Given that we do not allow direct reclaim to call us, we should
	 * never be called in a recursive filesystem reclaim context.
	 */
	if (WARN_ON_ONCE(current->flags & PF_MEMALLOC_NOFS))
		goto redirty;

	/*
	 * Is this page beyond the end of the file?
	 *
	 * The page index is less than the end_index, adjust the end_offset
	 * to the highest offset that this page should represent.
	 * -----------------------------------------------------
	 * |			file mapping	       | <EOF> |
	 * -----------------------------------------------------
	 * | Page ... | Page N-2 | Page N-1 |  Page N  |       |
	 * ^--------------------------------^----------|--------
	 * |     desired writeback range    |      see else    |
	 * ---------------------------------^------------------|
	 */
	offset = i_size_read(inode);
	end_index = offset >> PAGE_SHIFT;
	if (page->index < end_index)
		end_offset = (loff_t)(page->index + 1) << PAGE_SHIFT;
	else {
		/*
		 * Check whether the page to write out is beyond or straddles
		 * i_size or not.
		 * -------------------------------------------------------
		 * |		file mapping		        | <EOF>  |
		 * -------------------------------------------------------
		 * | Page ... | Page N-2 | Page N-1 |  Page N   | Beyond |
		 * ^--------------------------------^-----------|---------
		 * |				    |      Straddles     |
		 * ---------------------------------^-----------|--------|
		 */
		unsigned offset_into_page = offset & (PAGE_SIZE - 1);

		/*
		 * Skip the page if it is fully outside i_size, e.g. due to a
		 * truncate operation that is in progress. We must redirty the
		 * page so that reclaim stops reclaiming it. Otherwise
		 * iomap_vm_releasepage() is called on it and gets confused.
		 *
		 * Note that the end_index is unsigned long, it would overflow
		 * if the given offset is greater than 16TB on 32-bit system
		 * and if we do check the page is fully outside i_size or not
		 * via "if (page->index >= end_index + 1)" as "end_index + 1"
		 * will be evaluated to 0.  Hence this page will be redirtied
		 * and be written out repeatedly which would result in an
		 * infinite loop, the user program that perform this operation
		 * will hang.  Instead, we can verify this situation by checking
		 * if the page to write is totally beyond the i_size or if it's
		 * offset is just equal to the EOF.
		 */
		if (page->index > end_index ||
		    (page->index == end_index && offset_into_page == 0))
			goto redirty;

		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining
		 * memory is zeroed when mapped, and writes to that region are
		 * not written out to the file."
		 */
		zero_user_segment(page, offset_into_page, PAGE_SIZE);

		/* Adjust the end_offset to the end of file */
		end_offset = offset;
	}

	return iomap_writepage_map(wpc, wbc, inode, page, end_offset);

redirty:
	redirty_page_for_writepage(wbc, page);
	unlock_page(page);
	return 0;
}

int
iomap_writepage(struct page *page, struct writeback_control *wbc,
		struct iomap_writepage_ctx *wpc,
		const struct iomap_writeback_ops *ops)
{
	int ret;

	wpc->ops = ops;
	ret = iomap_do_writepage(page, wbc, wpc);
	if (!wpc->ioend)
		return ret;
	return iomap_submit_ioend(wpc, wpc->ioend, ret);
}
EXPORT_SYMBOL_GPL(iomap_writepage);

int
iomap_writepages(struct address_space *mapping, struct writeback_control *wbc,
		struct iomap_writepage_ctx *wpc,
		const struct iomap_writeback_ops *ops)
{
	int			ret;

	wpc->ops = ops;
	ret = write_cache_pages(mapping, wbc, iomap_do_writepage, wpc);
	if (!wpc->ioend)
		return ret;
	return iomap_submit_ioend(wpc, wpc->ioend, ret);
}
EXPORT_SYMBOL_GPL(iomap_writepages);

static int __init iomap_init(void)
{
	return bioset_init(&iomap_ioend_bioset, 4 * (PAGE_SIZE / SECTOR_SIZE),
			   offsetof(struct iomap_ioend, io_inline_bio),
			   BIOSET_NEED_BVECS);
}
fs_initcall(iomap_init);
