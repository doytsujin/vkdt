#pragma once
#include "core/core.h"
#include "dlist.h"
#include <stdint.h>
#include <assert.h>

// simple vulkan buffer memory allocator
// for the node graph. single thread use, not optimised in any sense.

typedef struct dt_vkmem_t
{
  uint64_t offset;   // to be uploaded as uniform/push const
  uint64_t size;     // only for us, the gpu will know what they asked for
  dt_vkmem_t *prev;  // for alloced/free lists
  dt_vkmem_t *next;
}
dt_vkmem_t;

typedef struct dt_vkalloc_t
{
  dt_vkmem_t *used;
  dt_vkmem_t *free;

  // fixed size pool of dt_vkmem_t to not fragment our real heap with this nonsense:
  uint64_t pool_size;
  dt_vkmem_t *vkmem_pool; // fixed size pool allocation
  dt_vkmem_t *unused;     // linked list into the above which are neither used nor free

  uint64_t peak_rss;
  uint64_t rss;
  uint64_t vmsize; // <= necessary to stay within limits here!
}
dt_vkalloc_t;

static inline void
dt_vkalloc_init(dt_vkalloc_t *a)
{
  const uint64_t heap_size = 1ul<<40; // doesn't matter, we'll never allocate this for real
  memset(a, 0, sizeof(*a));
  a->pool_size = 100;
  a->vkmem_pool = malloc(sizeof(dt_vkmem_t)*a->pool_size);
  memset(a->vkmem_pool, 0, sizeof(dt_vkmem_t)*a->pool_size); 
  a->free = DLIST_PREPEND(a->free, a->vkmem_pool);
  a->free->offset = 0;
  a->free->size = heap_size;
  for(int i=1;i<a->pool_size;i++)
    a->unused = DLIST_PREPEND(a->unused, a->vkmem_pool+i);
}

static inline void
dt_vkalloc_cleanup(dt_vkalloc_t *a)
{
  // free whole thing
  free(a->vkmem_pool);
  // don't free a, it's owned externally
  memset(a, 0, sizeof(*a));
}

// perform an (expensive) internal consistency check
static inline int
dt_vkalloc_check(dt_vkalloc_t *a)
{
  // count number of elements in linked lists
  uint64_t num_used = DLIST_LENGTH(a->used);
  uint64_t num_free = DLIST_LENGTH(a->free);
  uint64_t num_unused = DLIST_LENGTH(a->unused);
  if(num_used + num_free + num_unused != a->pool_size) return 1;

  // make sure free list refs sorted blocks of memory
  // also make sure they don't overlap
  dt_vkmem_t *l = a->free;
  uint64_t pos = 0, next_pos = 0;
  while(l)
  {
    if(l->offset < pos) return 2;
    if(l->offset + l->size < next_pos) return 3;
    pos = l->offset;
    next_pos = l->offset + l->size;
    l = l->next;
  }

  // make sure every pointer in pool is ref'd exactly once
  uint8_t *mark = calloc(sizeof(uint8_t)*a->pool_size);
  memset(mark, 0, sizeof(uint8_t)*a->pool_size);
  for(int i=0;i<3;i++)
  {
    l = a->used;
    if(i == 1) l = a->free;
    if(i == 2) l = a->unused;
    while(l)
    {
      int m = l - a->vkmem_pool;
      if(mark[m]) return 4; // already marked
      mark[m] = 1;
      l = l->next;
    }
  }
  for(int i=0;i<a->pool_size;i++)
    if(!mark[i]) return 5; // we lost one entry!

  // make sure no memory block is ref'd twice O(n^2)
  for(int i=0;i<2;i++)
  {
    l = a->used;
    if(i == 1) l = a->free;
    while(l)
    {
      for(int i=0;i<2;i++)
      {
        dt_vkmem_t l2 = a->used;
        if(i == 1) l2 = a->free;
        while(l2)
        {
          int good = 0;
          if(l->offset >= l2->offset + l2->size) good = 1;
          if(l->offset +  l->size <= l2->offset) good = 1;
          if(!good) return 6; // overlap detected!
          l2 = l2->next;
        }
      }
      l = l->next;
    }
  }
  return 0; // yay, we made it!
}

static inline dt_vkmem_t*
dt_vkalloc(dt_vkalloc_t *a, uint64_t size)
{
  // linear scan through free list O(n)
  dt_vkmem_t *l = a->free;
  while(l)
  {
    dt_vkmem_t *mem = 0;
    if(l->size == size)
    { // replace entry
      mem = l;
      DLIST_RM_ELEMENT(mem);
      if(l == a->free) a->free = l->next;
    }

    if(l->size > size)
    { // grab new mem entry from unused list
      assert(a->unused && "vkalloc: no more free slots!");
      if(!a->unused) return 0;
      mem = a->unused;
      a->unused = DLIST_REMOVE(a->unused, mem); // remove first is O(1)
      // split, push to used and modify free entry
      mem->offset = l->offset;
      mem->size = size;
      l->size -= size;
      l->offset += size;
    }

    if(mem)
    {
      a->rss += mem->size;
      a->peak_rss = MAX(a->peak_rss, a->rss);
      a->vmsize = MAX(a->vmsize, mem->offset + mem->size);
      a->used = DLIST_PREPEND(a->used, mem);
      return mem;
    }
    l = l->next;
  }
  return 0; // ouch, out of memory
}

static inline void
dt_vkfree(dt_vkalloc_t *a, dt_vkmem_t *mem)
{
  // remove from used list, put back to free list.
  a->rss -= mem->size;
  a->used = DLIST_REMOVE(a->used, mem);
  dt_vkmem_t *l = a->free;
  do
  {
    // keep sorted
    if(!l || l->offset >= mem->offset + mem->size)
    {
      dt_vkmem_t *t = DLIST_PREPEND(l, mem);
      if(l == a->free) a->free = t; // keep consistent
      // merge blocks:
      t = mem->prev;
      if(t)
      { // merge with before
        if(t->offset + t->size == mem->offset)
        {
          t->size += mem->size;
          DLIST_RM_ELEMENT(mem);
          a->unused = DLIST_PREPEND(a->unused, mem);
          mem = t;
        }
      }
      t = mem->next;
      if(t)
      { // merge with after
        if(t->offset == mem->offset + mem->size)
        {
          t->offset = mem->offset;
          t->size += mem->size;
          DLIST_RM_ELEMENT(mem);
          a->unused = DLIST_PREPEND(a->unused, mem);
          mem = t;
        }
      }
      return; // done
    }
  }
  while(l && (l = l->next));
  assert(0 && "vkalloc: inconsistent free list!");
}

