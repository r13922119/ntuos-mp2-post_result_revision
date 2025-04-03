#pragma once

#include "spinlock.h"
#include "types.h"
#include "list.h"

struct run {
  struct run *next;
};

// 2 * PGSHIFT_FOR_SLAB + 1 + MAGIC_SLAB_PADDING = 32
#define PGSHIFT_FOR_SLAB 12
#define MAGIC_SLAB_PADDING 7

/**
 * struct slab - Represents a slab in the slab allocator.
 * @link: with link.next and link.prev pointing to "link" of the other slabs in the list; also, it is an empty list only when newly created
 * @num_objs_in_use: number of allocated objects.
 * @freelist_front: Linked list start of free objects.
 * @freelist_rear: Linked list end of free objects.
 * @lazy_list_enabled: to indicate whether [slab + freelist_offset, slab + PGSIZE] is a free range without a real list of pointers
 * 
 * when freelist_front == 0, the slab is full; otherwise, it is partial or free
 * when num_objs_in_use == 0, the slab is free; otherwise, it is partial or full
 * 
 * the freelist is a queue, adopting a First-In-First-Out approach.
 * lazy_list_enabled is set to true when there are free objects in the slab never been allocated. they should form a range.
 * the freelist should be {slab+freelist_front, slab+freelist_front+obj_size, ..., slab+freelist_front+(MAX_OBJS-1)*object_size, A, B, ...},
 * where the addr of object A is stored at the object at 'slab+freelist_front+(MAX_OBJS-1) * object_size'; that of B is stored at A, and so on.
 * i.e., the freelist consists of a contiguous first part (till the end of the page) and a struct run* second part
 * it is designed to avoid traversing all the free objects when initializing the freelist
 */
struct slab
{
  // Link the slabs
  struct list_head link;
  // number of allocated objects.
  uint16 num_objs_in_use;
  union {
    struct {
      // Linked list start of free objects.
      unsigned freelist_front   : PGSHIFT_FOR_SLAB;
      // Linked list end of free objects.
      unsigned freelist_rear    : PGSHIFT_FOR_SLAB;
      // to indicate whether [slab + freelist_front, slab + PGSIZE] is a free range without a real list of pointers
      unsigned lazy_list_enabled: 1;
      // padded to 32
      unsigned reserved         : MAGIC_SLAB_PADDING;
    };
    uint32 list_meta;
  };
};

/**
 * struct kmem_cache - Represents a cache of slabs.
 * @name: Cache name (e.g., "file").
 * @object_size: Size of a single object.
 * @lock: Lock for cache management.
 * @full: Completely allocated slabs.
 * @partial: Partially allocated slabs.
 * @free: Free slabs.
 * @num_avail_slab: number of available ("partial" or "free") slabs.
 * @freelist_front: Linked list front of free objects.
 * @freelist_rear: Linked list rear of free objects.
 * @lazy_list_enabled: to indicate whether [slab + freelist_offset, slab + PGSIZE] is a free range without a real list of pointers. see struct slab for more info.
 * 
 * kmem_cache not only manages all the "full/partial/free" slabs with list_head. kmem_cache itself is also a slab, we fix its slab type label to "cache" instead of "full/partial/free"
 * however, no need "struct list_head cache;" since kmem_cache is the only slab of the "cache" type. also, no need "num_objs_in_use" since we never free "kmem_cache the slab". but we do need a freelist of objects like the other slabs!
 */
struct kmem_cache
{
  char name[32];        // Cache name (e.g., "file")
  uint object_size;     // Size of a single object
  struct spinlock lock; // Lock for cache management

  // Slab list(s)
  struct list_head full;     // Completely allocated slabs (Optional)
  struct list_head partial;  // Partially allocated slabs
  struct list_head free;     // Free slabs (Optional)

  uint32 num_avail_slab;
  union {
    struct {
      // Linked list front of free objects.
      unsigned freelist_front   : PGSHIFT_FOR_SLAB;
      // Linked list rear of free objects.
      unsigned freelist_rear    : PGSHIFT_FOR_SLAB;
      // to indicate whether [kmem_cache + freelist_front, kmem_cache + PGSIZE] is a free range
      unsigned lazy_list_enabled: 1;
      // padded to 32
      unsigned reserved         : MAGIC_SLAB_PADDING;
    };
    uint32 list_meta;
  };
};

/**
 * kmem_cache_create - Create a new slab cache.
 * @name: The name of the cache.
 * @object_size: The size of each object in the cache.
 *
 * Return: A pointer to the new cache.
 */
struct kmem_cache *kmem_cache_create(char *name, uint object_size);

/**
 * kmem_cache_destroy - Destroy a slab cache.
 * @cache: The cache to be destroyed.
 */
void kmem_cache_destroy(struct kmem_cache *cache);

/**
 * kmem_cache_alloc - Allocate an object from a slab cache.
 * @cache: The cache to allocate from.
 *
 * Return: A pointer to the allocated object.
 */
void *kmem_cache_alloc(struct kmem_cache *cache);

/**
 * kmem_cache_free - Free an object back to its slab cache.
 * @cache: The cache to free to.
 * @obj: The object to free.
 */
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/**
 * print_kmem_cache - Print the details of a kmem_cache.
 * @cache: The cache to print.
 * @print_fn: Function to print each object in the cache. If NULL (0) is given, will skip object printing part.
 */
void print_kmem_cache(struct kmem_cache *cache, void (*print_fn)(void *));
