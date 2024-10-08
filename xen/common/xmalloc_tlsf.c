/*
 * Two Levels Segregate Fit memory allocator (TLSF)
 * Version 2.3.2
 *
 * Written by Miguel Masmano Tello <mimastel@doctor.upv.es>
 *
 * Thanks to Ismael Ripoll for his suggestions and reviews
 *
 * Copyright (C) 2007, 2006, 2005, 2004
 *
 * This code is released using a dual license strategy: GPL/LGPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of the GNU General Public License Version 2.0
 * Released under the terms of the GNU Lesser General Public License 
 * Version 2.1
 *
 * This is kernel port of TLSF allocator.
 * Original code can be found at: http://rtportal.upv.es/rtmalloc/
 * Adapted for Linux by Nitin Gupta (nitingupta910@gmail.com)
 * (http://code.google.com/p/compcache/source/browse/trunk/sub-projects
 *  /allocators/tlsf-kmod r229 dated Aug 27, 2008
 * Adapted for Xen by Dan Magenheimer (dan.magenheimer@oracle.com)
 */

#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/pfn.h>
#include <asm/time.h>

#define MAX_POOL_NAME_LEN       16

/* Some IMPORTANT TLSF parameters */
#define MEM_ALIGN       (sizeof(void *) * 2)
#define MEM_ALIGN_MASK  (~(MEM_ALIGN - 1))

#define MAX_FLI         (30)
#define MAX_LOG2_SLI    (5)
#define MAX_SLI         (1 << MAX_LOG2_SLI)

#define FLI_OFFSET      (6)
/* tlsf structure just will manage blocks bigger than 128 bytes */
#define SMALL_BLOCK     (128)
#define REAL_FLI        (MAX_FLI - FLI_OFFSET)
#define MIN_BLOCK_SIZE  (sizeof(struct free_ptr))
#define BHDR_OVERHEAD   (sizeof(struct bhdr) - MIN_BLOCK_SIZE)

#define PTR_MASK        (sizeof(void *) - 1)
#define BLOCK_SIZE_MASK (0xFFFFFFFFU - PTR_MASK)

#define GET_NEXT_BLOCK(addr, r) ((struct bhdr *) \
                                ((char *)(addr) + (r)))
#define ROUNDUP_SIZE(r)         (((r) + MEM_ALIGN - 1) & MEM_ALIGN_MASK)
#define ROUNDDOWN_SIZE(r)       ((r) & MEM_ALIGN_MASK)
#define ROUNDUP_PAGE(r)         (((r) + PAGE_SIZE - 1) & PAGE_MASK)

#define BLOCK_STATE     (0x1)
#define PREV_STATE      (0x2)

/* bit 0 of the block size */
#define FREE_BLOCK      (0x1)
#define USED_BLOCK      (0x0)

/* bit 1 of the block size */
#define PREV_FREE       (0x2)
#define PREV_USED       (0x0)

static DEFINE_SPINLOCK(pool_list_lock);
static LIST_HEAD(pool_list_head);

struct free_ptr {
    struct bhdr *prev;
    struct bhdr *next;
};

struct bhdr {
    /* All blocks in a region are linked in order of physical address */
    struct bhdr *prev_hdr;
    /*
     * The size is stored in bytes
     *  bit 0: block is free, if set
     *  bit 1: previous block is free, if set
     */
    u32 size;
    /* Free blocks in individual freelists are linked */
    union {
        struct free_ptr free_ptr;
        u8 buffer[sizeof(struct free_ptr)];
    } ptr;
};

struct xmem_pool {
    /* First level bitmap (REAL_FLI bits) */
    u32 fl_bitmap;

    /* Second level bitmap */
    u32 sl_bitmap[REAL_FLI];

    /* Free lists */
    struct bhdr *matrix[REAL_FLI][MAX_SLI];

    spinlock_t lock;

    unsigned long max_size;
    unsigned long grow_size;

    /* Basic stats */
    unsigned long used_size;
    unsigned long num_regions;

    /* User provided functions for expanding/shrinking pool */
    xmem_pool_get_memory *get_mem;
    xmem_pool_put_memory *put_mem;

    struct list_head list;

    char name[MAX_POOL_NAME_LEN];
};

/*
 * Helping functions
 */

/**
 * Returns indexes (fl, sl) of the list used to serve request of size r
 */
static inline void MAPPING_SEARCH(unsigned long *r, int *fl, int *sl)
{
    int t;

    if ( *r < SMALL_BLOCK )
    {
        *fl = 0;
        *sl = *r / (SMALL_BLOCK / MAX_SLI);
    }
    else
    {
        t = (1 << (flsl(*r) - 1 - MAX_LOG2_SLI)) - 1;
        *r = *r + t;
        *fl = flsl(*r) - 1;
        *sl = (*r >> (*fl - MAX_LOG2_SLI)) - MAX_SLI;
        /* 
         * It's unclear what was the purpose of the commented-out code that now
         * is in the #else branch. The current form is motivated by the correction
         * of a violation MISRA:C 2012 Rule 3.1
         */
#if 1
        *fl -= FLI_OFFSET;
#else
        if ((*fl -= FLI_OFFSET) < 0) /* FL will be always >0! */
          *fl = *sl = 0;
#endif
        *r &= ~t;
    }
}

/**
 * Returns indexes (fl, sl) which is used as starting point to search
 * for a block of size r. It also rounds up requested size(r) to the
 * next list.
 */
static inline void MAPPING_INSERT(unsigned long r, int *fl, int *sl)
{
    if ( r < SMALL_BLOCK )
    {
        *fl = 0;
        *sl = r / (SMALL_BLOCK / MAX_SLI);
    }
    else
    {
        *fl = flsl(r) - 1;
        *sl = (r >> (*fl - MAX_LOG2_SLI)) - MAX_SLI;
        *fl -= FLI_OFFSET;
    }
}

/**
 * Returns first block from a list that hold blocks larger than or
 * equal to the one pointed by the indexes (fl, sl)
 */
static inline struct bhdr *FIND_SUITABLE_BLOCK(struct xmem_pool *p, int *fl,
                                               int *sl)
{
    u32 tmp = p->sl_bitmap[*fl] & (~0u << *sl);
    struct bhdr *b = NULL;

    if ( tmp )
    {
        *sl = ffs(tmp) - 1;
        b = p->matrix[*fl][*sl];
    }
    else
    {
        *fl = ffs(p->fl_bitmap & (~0u << (*fl + 1))) - 1;
        if ( likely(*fl > 0) )
        {
            *sl = ffs(p->sl_bitmap[*fl]) - 1;
            b = p->matrix[*fl][*sl];
        }
    }

    return b;
}

/**
 * Remove first free block(b) from free list with indexes (fl, sl).
 */
static inline void EXTRACT_BLOCK_HDR(struct bhdr *b, struct xmem_pool *p, int fl,
                                     int sl)
{
    p->matrix[fl][sl] = b->ptr.free_ptr.next;
    if ( p->matrix[fl][sl] )
    {
        p->matrix[fl][sl]->ptr.free_ptr.prev = NULL;
    }
    else
    {
        clear_bit(sl, &p->sl_bitmap[fl]);
        if ( !p->sl_bitmap[fl] )
            clear_bit(fl, &p->fl_bitmap);
    }
    b->ptr.free_ptr = (struct free_ptr) {NULL, NULL};
}

#define POISON_BYTE 0xAA

/**
 * Removes block(b) from free list with indexes (fl, sl)
 */
static inline void EXTRACT_BLOCK(struct bhdr *b, struct xmem_pool *p, int fl,
                                 int sl)
{
    if ( b->ptr.free_ptr.next )
        b->ptr.free_ptr.next->ptr.free_ptr.prev =
            b->ptr.free_ptr.prev;
    if ( b->ptr.free_ptr.prev )
        b->ptr.free_ptr.prev->ptr.free_ptr.next =
            b->ptr.free_ptr.next;
    if ( p->matrix[fl][sl] == b )
    {
        p->matrix[fl][sl] = b->ptr.free_ptr.next;
        if ( !p->matrix[fl][sl] )
        {
            clear_bit(sl, &p->sl_bitmap[fl]);
            if ( !p->sl_bitmap[fl] )
                clear_bit (fl, &p->fl_bitmap);
        }
    }
    b->ptr.free_ptr = (struct free_ptr) {NULL, NULL};

#ifdef CONFIG_XMEM_POOL_POISON
    if ( (b->size & BLOCK_SIZE_MASK) > MIN_BLOCK_SIZE )
        ASSERT(!memchr_inv(b->ptr.buffer + MIN_BLOCK_SIZE, POISON_BYTE,
                           (b->size & BLOCK_SIZE_MASK) - MIN_BLOCK_SIZE));
#endif /* CONFIG_XMEM_POOL_POISON */
}

/**
 * Insert block(b) in free list with indexes (fl, sl)
 */
static inline void INSERT_BLOCK(struct bhdr *b, struct xmem_pool *p, int fl, int sl)
{
#ifdef CONFIG_XMEM_POOL_POISON
    if ( (b->size & BLOCK_SIZE_MASK) > MIN_BLOCK_SIZE )
        memset(b->ptr.buffer + MIN_BLOCK_SIZE, POISON_BYTE,
               (b->size & BLOCK_SIZE_MASK) - MIN_BLOCK_SIZE);
#endif /* CONFIG_XMEM_POOL_POISON */

    b->ptr.free_ptr = (struct free_ptr) {NULL, p->matrix[fl][sl]};
    if ( p->matrix[fl][sl] )
        p->matrix[fl][sl]->ptr.free_ptr.prev = b;
    p->matrix[fl][sl] = b;
    set_bit(sl, &p->sl_bitmap[fl]);
    set_bit(fl, &p->fl_bitmap);
}

/**
 * Region is a virtually contiguous memory region and Pool is
 * collection of such regions
 */
static inline void ADD_REGION(void *region, unsigned long region_size,
                              struct xmem_pool *pool)
{
    int fl, sl;
    struct bhdr *b, *lb;

    b = (struct bhdr *)(region);
    b->prev_hdr = NULL;
    b->size = ROUNDDOWN_SIZE(region_size - 2 * BHDR_OVERHEAD)
        | FREE_BLOCK | PREV_USED;
    MAPPING_INSERT(b->size & BLOCK_SIZE_MASK, &fl, &sl);
    INSERT_BLOCK(b, pool, fl, sl);
    /* The sentinel block: allows us to know when we're in the last block */
    lb = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE_MASK);
    lb->prev_hdr = b;
    lb->size = 0 | USED_BLOCK | PREV_FREE;
    pool->used_size += BHDR_OVERHEAD; /* only sentinel block is "used" */
    pool->num_regions++;
}

/*
 * TLSF pool-based allocator start.
 */

struct xmem_pool *xmem_pool_create(
    const char *name,
    xmem_pool_get_memory get_mem,
    xmem_pool_put_memory put_mem,
    unsigned long max_size,
    unsigned long grow_size)
{
    struct xmem_pool *pool;
    int pool_bytes, pool_order;

    BUG_ON(max_size && (max_size < grow_size));

    pool_bytes = ROUNDUP_SIZE(sizeof(*pool));
    pool_order = get_order_from_bytes(pool_bytes);

    pool = (void *)alloc_xenheap_pages(pool_order, 0);
    if ( pool == NULL )
        return NULL;
    memset(pool, 0, pool_bytes);

    /* Round to next page boundary */
    max_size = ROUNDUP_PAGE(max_size);
    grow_size = ROUNDUP_PAGE(grow_size);

    /* pool global overhead not included in used size */
    pool->used_size = 0;

    pool->max_size = max_size;
    pool->grow_size = grow_size;
    pool->get_mem = get_mem;
    pool->put_mem = put_mem;
    strlcpy(pool->name, name, sizeof(pool->name));

    spin_lock_init(&pool->lock);

    spin_lock(&pool_list_lock);
    list_add_tail(&pool->list, &pool_list_head);
    spin_unlock(&pool_list_lock);

    return pool;
}

unsigned long xmem_pool_get_used_size(struct xmem_pool *pool)
{
    return pool->used_size;
}

unsigned long xmem_pool_get_total_size(struct xmem_pool *pool)
{
    unsigned long total;
    total = ROUNDUP_SIZE(sizeof(*pool))
        + (pool->num_regions - 1) * pool->grow_size;
    return total;
}

void xmem_pool_destroy(struct xmem_pool *pool) 
{
    int pool_bytes, pool_order;

    if ( pool == NULL )
        return;

    /* Check for memory leaks in this pool */
    if ( xmem_pool_get_used_size(pool) )
        printk("memory leak in pool: %s (%p). "
               "%lu bytes still in use.\n",
               pool->name, pool, xmem_pool_get_used_size(pool));

    spin_lock(&pool_list_lock);
    list_del_init(&pool->list);
    spin_unlock(&pool_list_lock);

    pool_bytes = ROUNDUP_SIZE(sizeof(*pool));
    pool_order = get_order_from_bytes(pool_bytes);
    free_xenheap_pages(pool,pool_order);
}

void *xmem_pool_alloc(unsigned long size, struct xmem_pool *pool)
{
    struct bhdr *b, *b2, *next_b, *region;
    int fl, sl;
    unsigned long tmp_size;

    ASSERT_ALLOC_CONTEXT();

    if ( size < MIN_BLOCK_SIZE )
        size = MIN_BLOCK_SIZE;
    else
    {
        tmp_size = ROUNDUP_SIZE(size);
        /* Guard against overflow. */
        if ( tmp_size < size )
            return NULL;
        size = tmp_size;
    }

    /* Rounding up the requested size and calculating fl and sl */

    spin_lock(&pool->lock);
 retry_find:
    MAPPING_SEARCH(&size, &fl, &sl);

    /* Searching a free block */
    if ( !(b = FIND_SUITABLE_BLOCK(pool, &fl, &sl)) )
    {
        /* Not found */
        if ( size > (pool->grow_size - 2 * BHDR_OVERHEAD) )
            goto out_locked;
        if ( pool->max_size && (pool->num_regions * pool->grow_size
                                > pool->max_size) )
            goto out_locked;
        spin_unlock(&pool->lock);
        if ( (region = pool->get_mem(pool->grow_size)) == NULL )
            goto out;
        spin_lock(&pool->lock);
        ADD_REGION(region, pool->grow_size, pool);
        goto retry_find;
    }
    EXTRACT_BLOCK_HDR(b, pool, fl, sl);

    /*-- found: */
    next_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE_MASK);
    /* Should the block be split? */
    tmp_size = (b->size & BLOCK_SIZE_MASK) - size;
    if ( tmp_size >= sizeof(struct bhdr) )
    {
        tmp_size -= BHDR_OVERHEAD;
        b2 = GET_NEXT_BLOCK(b->ptr.buffer, size);

        b2->size = tmp_size | FREE_BLOCK | PREV_USED;
        b2->prev_hdr = b;

        next_b->prev_hdr = b2;

        MAPPING_INSERT(tmp_size, &fl, &sl);
        INSERT_BLOCK(b2, pool, fl, sl);

        b->size = size | (b->size & PREV_STATE);
    }
    else
    {
        next_b->size &= (~PREV_FREE);
        b->size &= (~FREE_BLOCK); /* Now it's used */
    }

    pool->used_size += (b->size & BLOCK_SIZE_MASK) + BHDR_OVERHEAD;

    spin_unlock(&pool->lock);
    return (void *)b->ptr.buffer;

    /* Failed alloc */
 out_locked:
    spin_unlock(&pool->lock);

 out:
    return NULL;
}

void xmem_pool_free(void *ptr, struct xmem_pool *pool)
{
    struct bhdr *b, *tmp_b;
    int fl = 0, sl = 0;

    ASSERT_ALLOC_CONTEXT();

    if ( unlikely(ptr == NULL) )
        return;

    b = (struct bhdr *)((char *) ptr - BHDR_OVERHEAD);

    spin_lock(&pool->lock);
    b->size |= FREE_BLOCK;
    pool->used_size -= (b->size & BLOCK_SIZE_MASK) + BHDR_OVERHEAD;
    b->ptr.free_ptr = (struct free_ptr) { NULL, NULL};
    tmp_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE_MASK);
    if ( tmp_b->size & FREE_BLOCK )
    {
        MAPPING_INSERT(tmp_b->size & BLOCK_SIZE_MASK, &fl, &sl);
        EXTRACT_BLOCK(tmp_b, pool, fl, sl);
        b->size += (tmp_b->size & BLOCK_SIZE_MASK) + BHDR_OVERHEAD;
    }
    if ( b->size & PREV_FREE )
    {
        tmp_b = b->prev_hdr;
        MAPPING_INSERT(tmp_b->size & BLOCK_SIZE_MASK, &fl, &sl);
        EXTRACT_BLOCK(tmp_b, pool, fl, sl);
        tmp_b->size += (b->size & BLOCK_SIZE_MASK) + BHDR_OVERHEAD;
        b = tmp_b;
    }
    tmp_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE_MASK);
    tmp_b->prev_hdr = b;

    MAPPING_INSERT(b->size & BLOCK_SIZE_MASK, &fl, &sl);

    if ( (b->prev_hdr == NULL) && ((tmp_b->size & BLOCK_SIZE_MASK) == 0) )
    {
        pool->put_mem(b);
        pool->num_regions--;
        pool->used_size -= BHDR_OVERHEAD; /* sentinel block header */
        goto out;
    }

    INSERT_BLOCK(b, pool, fl, sl);

    tmp_b->size |= PREV_FREE;
    tmp_b->prev_hdr = b;
 out:
    spin_unlock(&pool->lock);
}

int xmem_pool_maxalloc(struct xmem_pool *pool)
{
    return pool->grow_size - (2 * BHDR_OVERHEAD);
}

/*
 * Glue for xmalloc().
 */

static struct xmem_pool *xenpool;

static void *cf_check xmalloc_pool_get(unsigned long size)
{
    ASSERT(size == PAGE_SIZE);
    return alloc_xenheap_page();
}

static void cf_check xmalloc_pool_put(void *p)
{
    free_xenheap_page(p);
}

static void *xmalloc_whole_pages(unsigned long size, unsigned long align)
{
    unsigned int i, order;
    void *res, *p;

    order = get_order_from_bytes(max(align, size));

    res = alloc_xenheap_pages(order, 0);
    if ( res == NULL )
        return NULL;

    for ( p = res + PAGE_ALIGN(size), i = 0; i < order; ++i )
        if ( (unsigned long)p & (PAGE_SIZE << i) )
        {
            free_xenheap_pages(p, i);
            p += PAGE_SIZE << i;
        }

    PFN_ORDER(virt_to_page(res)) = PFN_UP(size);
    /* Check that there was no truncation: */
    ASSERT(PFN_ORDER(virt_to_page(res)) == PFN_UP(size));

    return res;
}

static void tlsf_init(void)
{
    xenpool = xmem_pool_create("xmalloc", xmalloc_pool_get,
                               xmalloc_pool_put, 0, PAGE_SIZE);
    BUG_ON(!xenpool);
}

/*
 * xmalloc()
 */

static void *strip_padding(void *p)
{
    const struct bhdr *b = p - BHDR_OVERHEAD;

    if ( b->size & FREE_BLOCK )
    {
        p -= b->size & ~FREE_BLOCK;
        b = p - BHDR_OVERHEAD;
        ASSERT(!(b->size & FREE_BLOCK));
    }

    return p;
}

static void *add_padding(void *p, unsigned long align)
{
    unsigned int pad;

    if ( (pad = -(long)p & (align - 1)) != 0 )
    {
        void *q = p + pad;
        struct bhdr *b = q - BHDR_OVERHEAD;

        ASSERT(q > p);
        b->size = pad | FREE_BLOCK;
        p = q;
    }

    return p;
}

void *_xmalloc(unsigned long size, unsigned long align)
{
    void *p = NULL;

    ASSERT_ALLOC_CONTEXT();

    if ( !size )
        return ZERO_BLOCK_PTR;

    ASSERT((align & (align - 1)) == 0);
    if ( align < MEM_ALIGN )
        align = MEM_ALIGN;
    size += align - MEM_ALIGN;

    /* Guard against overflow. */
    if ( size < align - MEM_ALIGN )
        return NULL;

    if ( !xenpool )
        tlsf_init();

    if ( size < PAGE_SIZE )
        p = xmem_pool_alloc(size, xenpool);
    if ( p == NULL )
        return xmalloc_whole_pages(size - align + MEM_ALIGN, align);

    /* Add alignment padding. */
    p = add_padding(p, align);

    ASSERT(((unsigned long)p & (align - 1)) == 0);
    return p;
}

void *_xzalloc(unsigned long size, unsigned long align)
{
    void *p = _xmalloc(size, align);

    return p ? memset(p, 0, size) : p;
}

void *_xrealloc(void *ptr, unsigned long size, unsigned long align)
{
    unsigned long curr_size;
    void *p;

    if ( !size )
    {
        xfree(ptr);
        return ZERO_BLOCK_PTR;
    }

    if ( ptr == NULL || ptr == ZERO_BLOCK_PTR )
        return _xmalloc(size, align);

    ASSERT(!(align & (align - 1)));
    if ( align < MEM_ALIGN )
        align = MEM_ALIGN;

    if ( !((unsigned long)ptr & (PAGE_SIZE - 1)) )
    {
        curr_size = (unsigned long)PFN_ORDER(virt_to_page(ptr)) << PAGE_SHIFT;

        if ( size <= curr_size && !((unsigned long)ptr & (align - 1)) )
            return ptr;
    }
    else
    {
        unsigned long tmp_size = size + align - MEM_ALIGN;
        const struct bhdr *b;

        /* Guard against overflow. */
        if ( tmp_size < size )
            return NULL;

        if ( tmp_size < PAGE_SIZE )
            tmp_size = (tmp_size < MIN_BLOCK_SIZE) ? MIN_BLOCK_SIZE :
                ROUNDUP_SIZE(tmp_size);

        /* Strip alignment padding. */
        p = strip_padding(ptr);

        b = p - BHDR_OVERHEAD;
        curr_size = b->size & BLOCK_SIZE_MASK;

        if ( tmp_size <= curr_size )
        {
            /* Add alignment padding. */
            p = add_padding(p, align);

            ASSERT(!((unsigned long)p & (align - 1)));

            return p;
        }
    }

    p = _xmalloc(size, align);
    if ( p )
    {
        memcpy(p, ptr, min(curr_size, size));
        xfree(ptr);
    }

    return p;
}

void xfree(void *p)
{
    ASSERT_ALLOC_CONTEXT();

    if ( p == NULL || p == ZERO_BLOCK_PTR )
        return;

    if ( !((unsigned long)p & (PAGE_SIZE - 1)) )
    {
        unsigned long size = PFN_ORDER(virt_to_page(p));
        unsigned int i, order = get_order_from_pages(size);

        BUG_ON((unsigned long)p & ((PAGE_SIZE << order) - 1));
        PFN_ORDER(virt_to_page(p)) = 0;
        for ( i = 0; ; ++i )
        {
            if ( !(size & (1 << i)) )
                continue;
            size -= 1 << i;
            free_xenheap_pages(p + (size << PAGE_SHIFT), i);
            if ( i + 1 >= order )
                return;
        }
    }

    /* Strip alignment padding. */
    p = strip_padding(p);

    xmem_pool_free(p, xenpool);
}
