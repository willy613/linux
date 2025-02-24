/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEMAP_H
#define _LINUX_PAGEMAP_H

/*
 * Copyright 1995 Linus Torvalds
 */
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/compiler.h>
#include <linux/uaccess.h>
#include <linux/gfp.h>
#include <linux/bitops.h>
#include <linux/hardirq.h> /* for in_interrupt() */
#include <linux/hugetlb_inline.h>

struct pagevec;

/*
 * Bits in mapping->flags.
 */
enum mapping_flags {
	AS_EIO		= 0,	/* IO error on async write */
	AS_ENOSPC	= 1,	/* ENOSPC on async write */
	AS_MM_ALL_LOCKS	= 2,	/* under mm_take_all_locks() */
	AS_UNEVICTABLE	= 3,	/* e.g., ramdisk, SHM_LOCK */
	AS_EXITING	= 4, 	/* final truncate in progress */
	/* writeback related tags are not used */
	AS_NO_WRITEBACK_TAGS = 5,
	AS_MULTI_PAGE_FOLIOS = 6,
};

/**
 * mapping_set_error - record a writeback error in the address_space
 * @mapping: the mapping in which an error should be set
 * @error: the error to set in the mapping
 *
 * When writeback fails in some way, we must record that error so that
 * userspace can be informed when fsync and the like are called.  We endeavor
 * to report errors on any file that was open at the time of the error.  Some
 * internal callers also need to know when writeback errors have occurred.
 *
 * When a writeback error occurs, most filesystems will want to call
 * mapping_set_error to record the error in the mapping so that it can be
 * reported when the application calls fsync(2).
 */
static inline void mapping_set_error(struct address_space *mapping, int error)
{
	if (likely(!error))
		return;

	/* Record in wb_err for checkers using errseq_t based tracking */
	__filemap_set_wb_err(mapping, error);

	/* Record it in superblock */
	if (mapping->host)
		errseq_set(&mapping->host->i_sb->s_wb_err, error);

	/* Record it in flags for now, for legacy callers */
	if (error == -ENOSPC)
		set_bit(AS_ENOSPC, &mapping->flags);
	else
		set_bit(AS_EIO, &mapping->flags);
}

static inline void mapping_set_unevictable(struct address_space *mapping)
{
	set_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline void mapping_clear_unevictable(struct address_space *mapping)
{
	clear_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline bool mapping_unevictable(struct address_space *mapping)
{
	return mapping && test_bit(AS_UNEVICTABLE, &mapping->flags);
}

static inline void mapping_set_exiting(struct address_space *mapping)
{
	set_bit(AS_EXITING, &mapping->flags);
}

static inline int mapping_exiting(struct address_space *mapping)
{
	return test_bit(AS_EXITING, &mapping->flags);
}

static inline void mapping_set_no_writeback_tags(struct address_space *mapping)
{
	set_bit(AS_NO_WRITEBACK_TAGS, &mapping->flags);
}

static inline int mapping_use_writeback_tags(struct address_space *mapping)
{
	return !test_bit(AS_NO_WRITEBACK_TAGS, &mapping->flags);
}

static inline gfp_t mapping_gfp_mask(struct address_space * mapping)
{
	return mapping->gfp_mask;
}

/* Restricts the given gfp_mask to what the mapping allows. */
static inline gfp_t mapping_gfp_constraint(struct address_space *mapping,
		gfp_t gfp_mask)
{
	return mapping_gfp_mask(mapping) & gfp_mask;
}

/*
 * This is non-atomic.  Only to be used before the mapping is activated.
 * Probably needs a barrier...
 */
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{
	m->gfp_mask = mask;
}

static inline bool mapping_multi_page_folios(struct address_space *mapping)
{
	return test_bit(AS_MULTI_PAGE_FOLIOS, &mapping->flags);
}

static inline int filemap_nr_thps(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	return atomic_read(&mapping->nr_thps);
#else
	return 0;
#endif
}

static inline void filemap_nr_thps_inc(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	if (!mapping_multi_page_folios(mapping))
		atomic_inc(&mapping->nr_thps);
#else
	WARN_ON_ONCE(!mapping_multi_page_folios(mapping));
#endif
}

static inline void filemap_nr_thps_dec(struct address_space *mapping)
{
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	if (!mapping_multi_page_folios(mapping))
		atomic_dec(&mapping->nr_thps);
#else
	WARN_ON_ONCE(!mapping_multi_page_folios(mapping));
#endif
}

void release_folios(struct folio **folios, int nr);
void release_pages(struct page **pages, int nr);

/*
 * speculatively take a reference to a page.
 * If the page is free (_refcount == 0), then _refcount is untouched, and 0
 * is returned. Otherwise, _refcount is incremented by 1 and 1 is returned.
 *
 * This function must be called inside the same rcu_read_lock() section as has
 * been used to lookup the page in the pagecache radix-tree (or page table):
 * this allows allocators to use a synchronize_rcu() to stabilize _refcount.
 *
 * Unless an RCU grace period has passed, the count of all pages coming out
 * of the allocator must be considered unstable. page_count may return higher
 * than expected, and put_page must be able to do the right thing when the
 * page has been finished with, no matter what it is subsequently allocated
 * for (because put_page is what is used here to drop an invalid speculative
 * reference).
 *
 * This is the interesting part of the lockless pagecache (and lockless
 * get_user_pages) locking protocol, where the lookup-side (eg. find_get_page)
 * has the following pattern:
 * 1. find page in radix tree
 * 2. conditionally increment refcount
 * 3. check the page is still in pagecache (if no, goto 1)
 *
 * Remove-side that cares about stability of _refcount (eg. reclaim) has the
 * following (with the i_pages lock held):
 * A. atomically check refcount is correct and set it to 0 (atomic_cmpxchg)
 * B. remove page from pagecache
 * C. free the page
 *
 * There are 2 critical interleavings that matter:
 * - 2 runs before A: in this case, A sees elevated refcount and bails out
 * - A runs before 2: in this case, 2 sees zero refcount and retries;
 *   subsequently, B will complete and 1 will find no page, causing the
 *   lookup to return NULL.
 *
 * It is possible that between 1 and 2, the page is removed then the exact same
 * page is inserted into the same position in pagecache. That's OK: the
 * old find_get_page using a lock could equally have run before or after
 * such a re-insertion, depending on order that locks are granted.
 *
 * Lookups racing against pagecache insertion isn't a big problem: either 1
 * will find the page or it will not. Likewise, the old find_get_page could run
 * either before the insertion or afterwards, depending on timing.
 */
static inline int __page_cache_add_speculative(struct page *page, int count)
{
#ifdef CONFIG_TINY_RCU
# ifdef CONFIG_PREEMPT_COUNT
	VM_BUG_ON(!in_atomic() && !irqs_disabled());
# endif
	/*
	 * Preempt must be disabled here - we rely on rcu_read_lock doing
	 * this for us.
	 *
	 * Pagecache won't be truncated from interrupt context, so if we have
	 * found a page in the radix tree here, we have pinned its refcount by
	 * disabling preempt, and hence no need for the "speculative get" that
	 * SMP requires.
	 */
	VM_BUG_ON_PAGE(page_count(page) == 0, page);
	page_ref_add(page, count);

#else
	if (unlikely(!page_ref_add_unless(page, count, 0))) {
		/*
		 * Either the page has been freed, or will be freed.
		 * In either case, retry here and the caller should
		 * do the right thing (see comments above).
		 */
		return 0;
	}
#endif
	VM_BUG_ON_PAGE(PageTail(page), page);

	return 1;
}

static inline int page_cache_get_speculative(struct page *page)
{
	return __page_cache_add_speculative(page, 1);
}

static inline int page_cache_add_speculative(struct page *page, int count)
{
	return __page_cache_add_speculative(page, count);
}

/**
 * attach_page_private - Attach private data to a page.
 * @page: Page to attach data to.
 * @data: Data to attach to page.
 *
 * Attaching private data to a page increments the page's reference count.
 * The data must be detached before the page will be freed.
 */
static inline void attach_page_private(struct page *page, void *data)
{
	get_page(page);
	set_page_private(page, (unsigned long)data);
	SetPagePrivate(page);
}

/**
 * detach_page_private - Detach private data from a page.
 * @page: Page to detach data from.
 *
 * Removes the data that was previously attached to the page and decrements
 * the refcount on the page.
 *
 * Return: Data that was attached to the page.
 */
static inline void *detach_page_private(struct page *page)
{
	void *data = (void *)page_private(page);

	if (!PagePrivate(page))
		return NULL;
	ClearPagePrivate(page);
	set_page_private(page, 0);
	put_page(page);

	return data;
}

#ifdef CONFIG_NUMA
extern struct folio *__page_cache_alloc(gfp_t gfp, unsigned int order);
#else
static inline struct folio *__page_cache_alloc(gfp_t gfp, unsigned int order)
{
	return alloc_folio(gfp, order);
}
#endif

static inline struct page *page_cache_alloc(struct address_space *x)
{
	return &__page_cache_alloc(mapping_gfp_mask(x), 0)->page;
}

static inline gfp_t readahead_gfp_mask(struct address_space *x)
{
	return mapping_gfp_mask(x) | __GFP_NORETRY | __GFP_NOWARN;
}

typedef int filler_t(void *, struct folio *);

pgoff_t page_cache_next_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan);
pgoff_t page_cache_prev_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan);

#define FGP_ACCESSED		0x00000001
#define FGP_LOCK		0x00000002
#define FGP_CREAT		0x00000004
#define FGP_WRITE		0x00000008
#define FGP_NOFS		0x00000010
#define FGP_NOWAIT		0x00000020
#define FGP_FOR_MMAP		0x00000040
#define FGP_ENTRY		0x00000080

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp);
struct page *pagecache_get_page(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp);

/**
 * find_get_page - find and get a page reference
 * @mapping: the address_space to search
 * @offset: the page index
 *
 * Looks up the page cache slot at @mapping & @offset.  If there is a
 * page cache page, it is returned with an increased refcount.
 *
 * Otherwise, %NULL is returned.
 */
static inline struct page *find_get_page(struct address_space *mapping,
					pgoff_t offset)
{
	return pagecache_get_page(mapping, offset, 0, 0);
}

static inline struct folio *find_get_folio(struct address_space *mapping,
					pgoff_t index)
{
	return filemap_get_folio(mapping, index, 0, 0);
}

static inline struct page *find_get_page_flags(struct address_space *mapping,
					pgoff_t offset, int fgp_flags)
{
	return pagecache_get_page(mapping, offset, fgp_flags, 0);
}

/**
 * find_lock_page - locate, pin and lock a pagecache page
 * @mapping: the address_space to search
 * @index: the page index
 *
 * Looks up the page cache entry at @mapping & @index.  If there is a
 * page cache page, it is returned locked and with an increased
 * refcount.
 *
 * Context: May sleep.
 * Return: A struct page or %NULL if there is no page in the cache for this
 * index.
 */
static inline struct page *find_lock_page(struct address_space *mapping,
					pgoff_t index)
{
	return pagecache_get_page(mapping, index, FGP_LOCK, 0);
}

/**
 * find_or_create_page - locate or add a pagecache page
 * @mapping: the page's address_space
 * @index: the page's index into the mapping
 * @gfp_mask: page allocation mode
 *
 * Looks up the page cache slot at @mapping & @offset.  If there is a
 * page cache page, it is returned locked and with an increased
 * refcount.
 *
 * If the page is not present, a new page is allocated using @gfp_mask
 * and added to the page cache and the VM's LRU list.  The page is
 * returned locked and with an increased refcount.
 *
 * On memory exhaustion, %NULL is returned.
 *
 * find_or_create_page() may sleep, even if @gfp_flags specifies an
 * atomic allocation!
 */
static inline struct page *find_or_create_page(struct address_space *mapping,
					pgoff_t index, gfp_t gfp_mask)
{
	return pagecache_get_page(mapping, index,
					FGP_LOCK|FGP_ACCESSED|FGP_CREAT,
					gfp_mask);
}

/**
 * grab_cache_page_nowait - returns locked page at given index in given cache
 * @mapping: target address_space
 * @index: the page index
 *
 * Same as grab_cache_page(), but do not wait if the page is unavailable.
 * This is intended for speculative data generators, where the data can
 * be regenerated if the page couldn't be grabbed.  This routine should
 * be safe to call while holding the lock for another page.
 *
 * Clear __GFP_FS when allocating the page to avoid recursion into the fs
 * and deadlock against the caller's locked page.
 */
static inline struct page *grab_cache_page_nowait(struct address_space *mapping,
				pgoff_t index)
{
	return pagecache_get_page(mapping, index,
			FGP_LOCK|FGP_CREAT|FGP_NOFS|FGP_NOWAIT,
			mapping_gfp_mask(mapping));
}

/* Does this page contain this index? */
static inline bool thp_contains(struct page *head, pgoff_t index)
{
	/* HugeTLBfs indexes the page cache in units of hpage_size */
	if (PageHuge(head))
		return head->index == index;
	return page_index(head) == (index & ~(thp_nr_pages(head) - 1UL));
}

static inline pgoff_t folio_index(struct folio *folio)
{
        if (unlikely(FolioSwapCache(folio)))
                return __page_file_index(&folio->page);
        return folio->page.index;
}

static inline pgoff_t folio_next_index(struct folio *folio)
{
	return folio->page.index + folio_nr_pages(folio);
}

static inline struct page *folio_page(struct folio *folio, pgoff_t index)
{
	index -= folio_index(folio);
	VM_BUG_ON_PAGE(index >= folio_nr_pages(folio), &folio->page);
	return &folio->page + index;
}

/* Does this folio contain this index? */
static inline bool folio_contains(struct folio *folio, pgoff_t index)
{
	/* HugeTLBfs indexes the page cache in units of hpage_size */
	if (PageHuge(&folio->page))
		return folio->page.index == index;
	return index - folio_index(folio) < folio_nr_pages(folio);
}

/*
 * Given the page we found in the page cache, return the page corresponding
 * to this index in the file
 */
static inline struct page *find_subpage(struct page *head, pgoff_t index)
{
	/* HugeTLBfs wants the head page regardless */
	if (PageHuge(head))
		return head;

	return head + (index & (thp_nr_pages(head) - 1));
}

unsigned find_get_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct pagevec *pvec, pgoff_t *indices);
unsigned find_get_pages_range(struct address_space *mapping, pgoff_t *start,
			pgoff_t end, unsigned int nr_pages,
			struct page **pages);
static inline unsigned find_get_pages(struct address_space *mapping,
			pgoff_t *start, unsigned int nr_pages,
			struct page **pages)
{
	return find_get_pages_range(mapping, start, (pgoff_t)-1, nr_pages,
				    pages);
}
unsigned find_get_pages_contig(struct address_space *mapping, pgoff_t start,
			       unsigned int nr_pages, struct page **pages);
unsigned find_get_pages_range_tag(struct address_space *mapping, pgoff_t *index,
			pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages);
static inline unsigned find_get_pages_tag(struct address_space *mapping,
			pgoff_t *index, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages)
{
	return find_get_pages_range_tag(mapping, index, (pgoff_t)-1, tag,
					nr_pages, pages);
}

struct folio *filemap_get_stable_folio(struct address_space *mapping,
		pgoff_t index, unsigned flags);
struct page *grab_cache_page_write_begin(struct address_space *mapping,
			pgoff_t index, unsigned flags);

/*
 * Returns locked page at given index in given cache, creating it if needed.
 */
static inline struct page *grab_cache_page(struct address_space *mapping,
								pgoff_t index)
{
	return find_or_create_page(mapping, index, mapping_gfp_mask(mapping));
}

struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
		filler_t *filler, void *data);
extern struct page * read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index, gfp_t gfp_mask);
extern int read_cache_pages(struct address_space *mapping,
		struct list_head *pages, filler_t *filler, void *data);

static inline struct page *read_cache_page(struct address_space *mapping,
				pgoff_t index, filler_t *filler, void *data)
{
	struct folio *folio = read_cache_folio(mapping, index, filler, data);
	if (IS_ERR(folio))
		return &folio->page;
	return folio_page(folio, index);
}

static inline struct page *read_mapping_page(struct address_space *mapping,
				pgoff_t index, void *data)
{
	return read_cache_page(mapping, index, NULL, data);
}

static inline struct folio *read_mapping_folio(struct address_space *mapping,
				pgoff_t index, void *data)
{
	return read_cache_folio(mapping, index, NULL, data);
}

/*
 * Get index of the page with in radix-tree
 * (TODO: remove once hugetlb pages will have ->index in PAGE_SIZE)
 */
static inline pgoff_t page_to_index(struct page *page)
{
	pgoff_t pgoff;

	if (likely(!PageTransTail(page)))
		return page->index;

	/*
	 *  We don't initialize ->index for tail pages: calculate based on
	 *  head page
	 */
	pgoff = compound_head(page)->index;
	pgoff += page - compound_head(page);
	return pgoff;
}

/*
 * Get the offset in PAGE_SIZE.
 * (TODO: hugepage should have ->index in PAGE_SIZE)
 */
static inline pgoff_t page_to_pgoff(struct page *page)
{
	if (unlikely(PageHeadHuge(page)))
		return page->index << compound_order(page);

	return page_to_index(page);
}

/*
 * Return byte-offset into filesystem object for page.
 */
static inline loff_t page_offset(struct page *page)
{
	return ((loff_t)page->index) << PAGE_SHIFT;
}

static inline loff_t page_file_offset(struct page *page)
{
	return ((loff_t)page_index(page)) << PAGE_SHIFT;
}

static inline loff_t folio_offset(struct folio *folio)
{
	return page_offset(&folio->page);
}

static inline loff_t folio_file_offset(struct folio *folio)
{
	return page_file_offset(&folio->page);
}

extern pgoff_t linear_hugepage_index(struct vm_area_struct *vma,
				     unsigned long address);

static inline pgoff_t linear_page_index(struct vm_area_struct *vma,
					unsigned long address)
{
	pgoff_t pgoff;
	if (unlikely(is_vm_hugetlb_page(vma)))
		return linear_hugepage_index(vma, address);
	pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
	pgoff += vma->vm_pgoff;
	return pgoff;
}

/* This has the same layout as wait_bit_key - see fs/cachefiles/rdwr.c */
struct wait_page_key {
	struct page *page;
	int bit_nr;
	int page_match;
};

struct wait_page_queue {
	struct page *page;
	int bit_nr;
	wait_queue_entry_t wait;
};

static inline bool wake_page_match(struct wait_page_queue *wait_page,
				  struct wait_page_key *key)
{
	if (wait_page->page != key->page)
	       return false;
	key->page_match = 1;

	if (wait_page->bit_nr != key->bit_nr)
		return false;

	return true;
}

void __lock_folio(struct folio *folio);
int __lock_folio_killable(struct folio *folio);
int __lock_folio_async(struct folio *folio, struct wait_page_queue *wait);
int __lock_folio_or_retry(struct folio *folio, struct mm_struct *mm,
				unsigned int flags);
void unlock_folio(struct folio *folio);

/**
 * unlock_page - Unlock a locked page.
 * @page: The page.
 *
 * Unlocks the page and wakes up any thread sleeping on the page lock.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
static inline void unlock_page(struct page *page)
{
	return unlock_folio(page_folio(page));
}

static inline bool trylock_folio(struct folio *folio)
{
	return likely(!test_and_set_bit_lock(PG_locked, folio_flags(folio)));
}

/*
 * Return true if the page was successfully locked
 */
static inline int trylock_page(struct page *page)
{
	return trylock_folio(page_folio(page));
}

static inline void lock_folio(struct folio *folio)
{
	might_sleep();
	if (!trylock_folio(folio))
		__lock_folio(folio);
}

/*
 * lock_page may only be called if we have the page's inode pinned.
 */
static inline void lock_page(struct page *page)
{
	lock_folio(page_folio(page));
}

static inline int lock_folio_killable(struct folio *folio)
{
	might_sleep();
	if (!trylock_folio(folio))
		return __lock_folio_killable(folio);
	return 0;
}

/*
 * lock_page_killable is like lock_page but can be interrupted by fatal
 * signals.  It returns 0 if it locked the page and -EINTR if it was
 * killed while waiting.
 */
static inline int lock_page_killable(struct page *page)
{
	return lock_folio_killable(page_folio(page));
}

/*
 * lock_folio_async - Lock the folio, unless this would block. If the folio
 * is already locked, then queue a callback when the folio becomes unlocked.
 * This callback can then retry the operation.
 *
 * Returns 0 if the folio is locked successfully, or -EIOCBQUEUED if the folio
 * was already locked and the callback defined in 'wait' was queued.
 */
static inline int lock_folio_async(struct folio *folio,
				  struct wait_page_queue *wait)
{
	if (!trylock_folio(folio))
		return __lock_folio_async(folio, wait);
	return 0;
}

/*
 * lock_folio_or_retry - Lock the folio, unless this would block and the
 * caller indicated that it can handle a retry.
 *
 * Return value and mmap_lock implications depend on flags; see
 * __lock_folio_or_retry().
 */
static inline int lock_folio_or_retry(struct folio *folio, struct mm_struct *mm,
				     unsigned int flags)
{
	might_sleep();
	return trylock_folio(folio) || __lock_folio_or_retry(folio, mm, flags);
}

/*
 * This is exported only for wait_on_page_locked/wait_on_page_writeback, etc.,
 * and should not be used directly.
 */
extern void wait_on_folio_bit(struct folio *folio, int bit_nr);
extern int wait_on_folio_bit_killable(struct folio *folio, int bit_nr);

/* 
 * Wait for a page to be unlocked.
 *
 * This must be called with the caller "holding" the page,
 * ie with increased "page->count" so that the page won't
 * go away during the wait..
 */
static inline void wait_on_folio_locked(struct folio *folio)
{
	if (FolioLocked(folio))
		wait_on_folio_bit(folio, PG_locked);
}

static inline int wait_on_folio_locked_killable(struct folio *folio)
{
	if (!FolioLocked(folio))
		return 0;
	return wait_on_folio_bit_killable(folio, PG_locked);
}

static inline void wait_on_page_locked(struct page *page)
{
	wait_on_folio_locked(page_folio(page));
}

static inline int wait_on_page_locked_killable(struct page *page)
{
	return wait_on_folio_locked_killable(page_folio(page));
}

extern void put_and_wait_on_page_locked(struct page *page);

void wait_on_folio_writeback(struct folio *folio);
static inline void wait_on_page_writeback(struct page *page)
{
	return wait_on_folio_writeback(page_folio(page));
}
void end_folio_writeback(struct folio *folio);
static inline void end_page_writeback(struct page *page)
{
	return end_folio_writeback(page_folio(page));
}
void wait_for_stable_folio(struct folio *folio);
static inline void wait_for_stable_page(struct page *page)
{
	return wait_for_stable_folio(page_folio(page));
}

void page_endio(struct page *page, bool is_write, int err);

/*
 * Add an arbitrary waiter to a page's wait queue
 */
extern void add_page_wait_queue(struct page *page, wait_queue_entry_t *waiter);

/*
 * Fault everything in given userspace address range in.
 */
static inline int fault_in_pages_writeable(char __user *uaddr, int size)
{
	char __user *end = uaddr + size - 1;

	if (unlikely(size == 0))
		return 0;

	if (unlikely(uaddr > end))
		return -EFAULT;
	/*
	 * Writing zeroes into userspace here is OK, because we know that if
	 * the zero gets there, we'll be overwriting it.
	 */
	do {
		if (unlikely(__put_user(0, uaddr) != 0))
			return -EFAULT;
		uaddr += PAGE_SIZE;
	} while (uaddr <= end);

	/* Check whether the range spilled into the next page. */
	if (((unsigned long)uaddr & PAGE_MASK) ==
			((unsigned long)end & PAGE_MASK))
		return __put_user(0, end);

	return 0;
}

static inline int fault_in_pages_readable(const char __user *uaddr, int size)
{
	volatile char c;
	const char __user *end = uaddr + size - 1;

	if (unlikely(size == 0))
		return 0;

	if (unlikely(uaddr > end))
		return -EFAULT;

	do {
		if (unlikely(__get_user(c, uaddr) != 0))
			return -EFAULT;
		uaddr += PAGE_SIZE;
	} while (uaddr <= end);

	/* Check whether the range spilled into the next page. */
	if (((unsigned long)uaddr & PAGE_MASK) ==
			((unsigned long)end & PAGE_MASK)) {
		return __get_user(c, end);
	}

	(void)c;
	return 0;
}

int add_to_page_cache_locked(struct page *page, struct address_space *mapping,
				pgoff_t index, gfp_t gfp);
int folio_add_to_page_cache(struct folio *folio, struct address_space *mapping,
				pgoff_t index, gfp_t gfp);
extern void delete_from_page_cache(struct page *page);
extern void __delete_from_page_cache(struct page *page, void *shadow);
int replace_page_cache_page(struct page *old, struct page *new, gfp_t gfp_mask);
void delete_from_page_cache_batch(struct address_space *mapping,
				  struct pagevec *pvec);
loff_t mapping_seek_hole_data(struct address_space *, loff_t start, loff_t end,
		int whence);

/*
 * Like add_to_page_cache_locked, but used to add newly allocated pages:
 * the page is new, so we can just run __SetPageLocked() against it.
 */
static inline int add_to_page_cache(struct page *page,
		struct address_space *mapping, pgoff_t offset, gfp_t gfp_mask)
{
	int error;

	__SetPageLocked(page);
	error = add_to_page_cache_locked(page, mapping, offset, gfp_mask);
	if (unlikely(error))
		__ClearPageLocked(page);
	return error;
}

static inline int add_to_page_cache_lru(struct page *page,
		struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	return folio_add_to_page_cache((struct folio *)page, mapping,
			index, gfp);
}

/**
 * struct readahead_control - Describes a readahead request.
 *
 * A readahead request is for consecutive pages.  Filesystems which
 * implement the ->readahead method should call readahead_folio() or
 * readahead_folio_batch() in a loop and attempt to start I/O against
 * each page in the request.
 *
 * Most of the fields in this struct are private and should be accessed
 * by the functions below.
 *
 * @file: The file, used primarily by network filesystems for authentication.
 *	  May be NULL if invoked internally by the filesystem.
 * @mapping: Readahead this filesystem object.
 */
struct readahead_control {
	struct file *file;
	struct address_space *mapping;
/* private: use the readahead_* accessors instead */
	pgoff_t _index;
	unsigned int _nr_pages;
	unsigned int _batch_count;
};

#define DEFINE_READAHEAD(rac, f, m, i)					\
	struct readahead_control rac = {				\
		.file = f,						\
		.mapping = m,						\
		._index = i,						\
	}

#define VM_READAHEAD_PAGES	(SZ_128K / PAGE_SIZE)

void page_cache_ra_unbounded(struct readahead_control *,
		unsigned long nr_to_read, unsigned long lookahead_count);
void page_cache_sync_ra(struct readahead_control *, struct file_ra_state *,
		unsigned long req_count);
void page_cache_async_ra(struct readahead_control *, struct file_ra_state *,
		struct page *, unsigned long req_count);

/**
 * page_cache_sync_readahead - generic file readahead
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @file: Used by the filesystem for authentication.
 * @index: Index of first page to be read.
 * @req_count: Total number of pages being read by the caller.
 *
 * page_cache_sync_readahead() should be called when a cache miss happened:
 * it will submit the read.  The readahead logic may decide to piggyback more
 * pages onto the read request if access patterns suggest it will improve
 * performance.
 */
static inline
void page_cache_sync_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *file, pgoff_t index,
		unsigned long req_count)
{
	DEFINE_READAHEAD(ractl, file, mapping, index);
	page_cache_sync_ra(&ractl, ra, req_count);
}

/**
 * page_cache_async_readahead - file readahead for marked pages
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @file: Used by the filesystem for authentication.
 * @page: The page at @index which triggered the readahead call.
 * @index: Index of first page to be read.
 * @req_count: Total number of pages being read by the caller.
 *
 * page_cache_async_readahead() should be called when a page is used which
 * is marked as PageReadahead; this is a marker to suggest that the application
 * has used up enough of the readahead window that we should start pulling in
 * more pages.
 */
static inline
void page_cache_async_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *file,
		struct page *page, pgoff_t index, unsigned long req_count)
{
	DEFINE_READAHEAD(ractl, file, mapping, index);
	page_cache_async_ra(&ractl, ra, page, req_count);
}

/**
 * readahead_folio - Get the next folio to read.
 * @rac: The current readahead request.
 *
 * Context: The folio is locked.  The caller should unlock the folio once
 * all I/O to that folio has completed.
 * Return: A pointer to the next folio, or %NULL if we are done.
 */
static inline struct folio *readahead_folio(struct readahead_control *rac)
{
	struct folio *folio;

	BUG_ON(rac->_batch_count > rac->_nr_pages);
	rac->_nr_pages -= rac->_batch_count;
	rac->_index += rac->_batch_count;

	if (!rac->_nr_pages) {
		rac->_batch_count = 0;
		return NULL;
	}

	folio = xa_load(&rac->mapping->i_pages, rac->_index);
	VM_BUG_ON_PAGE(!FolioLocked(folio), &folio->page);
	rac->_batch_count = folio_nr_pages(folio);

	return folio;
}

static inline unsigned int __readahead_batch(struct readahead_control *rac,
		struct folio **array, unsigned int array_sz)
{
	unsigned int i = 0;
	XA_STATE(xas, &rac->mapping->i_pages, 0);
	struct folio *folio;

	BUG_ON(rac->_batch_count > rac->_nr_pages);
	rac->_nr_pages -= rac->_batch_count;
	rac->_index += rac->_batch_count;
	rac->_batch_count = 0;

	xas_set(&xas, rac->_index);
	rcu_read_lock();
	xas_for_each(&xas, folio, rac->_index + rac->_nr_pages - 1) {
		if (xas_retry(&xas, folio))
			continue;
		VM_BUG_ON_PAGE(!FolioLocked(folio), &folio->page);
		array[i++] = folio;
		rac->_batch_count += folio_nr_pages(folio);

		/*
		 * The page cache isn't using multi-index entries yet,
		 * so the xas cursor needs to be manually moved to the
		 * next index.  This can be removed once the page cache
		 * is converted.
		 */
		if (FolioMulti(folio))
			xas_set(&xas, rac->_index + rac->_batch_count);

		if (i == array_sz)
			break;
	}
	rcu_read_unlock();

	return i;
}

/**
 * readahead_folio_batch - Get a batch of folios to read.
 * @rac: The current readahead request.
 * @array: An array of pointers to struct folio.
 *
 * Context: The folios are locked.  The caller should unlock the folio
 * once all I/O to that folio has completed.
 * Return: The number of folios placed in the array.  0 indicates the request
 * is complete.
 */
#define readahead_folio_batch(rac, array)				\
	__readahead_batch(rac, array, ARRAY_SIZE(array))

/**
 * readahead_pos - The byte offset into the file of this readahead request.
 * @rac: The readahead request.
 */
static inline loff_t readahead_pos(struct readahead_control *rac)
{
	return (loff_t)rac->_index * PAGE_SIZE;
}

/**
 * readahead_length - The number of bytes in this readahead request.
 * @rac: The readahead request.
 */
static inline loff_t readahead_length(struct readahead_control *rac)
{
	return (loff_t)rac->_nr_pages * PAGE_SIZE;
}

/**
 * readahead_index - The index of the first page in this readahead request.
 * @rac: The readahead request.
 */
static inline pgoff_t readahead_index(struct readahead_control *rac)
{
	return rac->_index;
}

/**
 * readahead_count - The number of pages in this readahead request.
 * @rac: The readahead request.
 */
static inline unsigned int readahead_count(struct readahead_control *rac)
{
	return rac->_nr_pages;
}

/**
 * readahead_batch_length - The number of bytes in the current batch.
 * @rac: The readahead request.
 */
static inline loff_t readahead_batch_length(struct readahead_control *rac)
{
	return rac->_batch_count * PAGE_SIZE;
}

static inline unsigned long dir_pages(struct inode *inode)
{
	return (unsigned long)(inode->i_size + PAGE_SIZE - 1) >>
			       PAGE_SHIFT;
}

/**
 * folio_mkwrite_check_truncate - check if page was truncated
 * @folio: the page to check
 * @inode: the inode to check the page against
 *
 * Return: The number of bytes in the folio up to EOF,
 * or -EFAULT if the page was truncated.
 */
static inline ssize_t folio_mkwrite_check_truncate(struct folio *folio,
					      struct inode *inode)
{
	loff_t size = i_size_read(inode);
	pgoff_t end_index = size >> PAGE_SHIFT;
	pgoff_t index = folio_index(folio);
	size_t offset = offset_in_folio(folio, size);

	if (!folio->page.mapping)
		return -EFAULT;

	/* page is wholly inside EOF */
	if (index + folio_nr_pages(folio) - 1 < end_index)
		return folio_size(folio);
	/* page is wholly past EOF */
	if (index > end_index || !offset)
		return -EFAULT;
	/* page is partially inside EOF */
	return offset;
}

/**
 * i_blocks_per_folio - How many blocks fit in this folio.
 * @inode: The inode which contains the blocks.
 * @folio: The folio.
 *
 * If the block size is larger than the size of this folio, returns zero.
 *
 * Context: The caller should hold a refcount on the folio to prevent it
 * from being split.
 * Return: The number of filesystem blocks covered by this folio.
 */
static inline
unsigned int i_blocks_per_folio(struct inode *inode, struct folio *folio)
{
	return folio_size(folio) >> inode->i_blkbits;
}
#endif /* _LINUX_PAGEMAP_H */
