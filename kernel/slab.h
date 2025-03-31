#pragma once

#include "spinlock.h"
#include "types.h"
#include "list.h"

struct run {
  struct run *next;
};

// 2 * PGSHIFT_FOR_SLAB + MAGIC_SLAB_PADDING  = 32
// 1 * PGSHIFT_FOR_SLAB + MAGIC_CACHE_PADDING = 16
#define PGSHIFT_FOR_SLAB 12
#define MAGIC_SLAB_PADDING 8
#define MAGIC_CACHE_PADDING 4

/**
 * struct slab - Represents a slab in the slab allocator.
 * @freelist_offset: Linked list of free objects.
 * @num_objs_in_use: number of allocated objects.
 * @link: with link.next and link.prev pointing to "link" of the other slabs in the list; also, it is an empty list only when newly created
 * 
 * when freelist_offset == 0, the slab is full; otherwise, it is partial or free
 * when num_objs_in_use == 0, the slab is free; otherwise, it is partial or full
 */
struct slab
{
  union {
    struct {
      // Linked list of free objects.
      unsigned freelist_offset : PGSHIFT_FOR_SLAB;
      // number of allocated objects.
      unsigned num_objs_in_use : PGSHIFT_FOR_SLAB;
      // padded to 32
      unsigned reserved        : MAGIC_SLAB_PADDING;
    };
    uint32 meta;
  };
  // Link the slabs
  struct list_head link;
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
 * @freelist_offset: linked list of free objects.
 * 
 * kmem_cache not only mangages all the "full/partial/free" slabs with list_head. kmem_cache itself is also a slab, we fix its slab type label to "cache" instead of "full/partial/free"
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
      // Linked list of free objects.
      unsigned freelist_offset : PGSHIFT_FOR_SLAB;
      // padded to 16
      unsigned reserved        : MAGIC_CACHE_PADDING;
    };
    uint16 cache_slab_meta;
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
