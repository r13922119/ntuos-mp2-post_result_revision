#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"
#include "debug.h"
#include "list.h"

enum STATE{
  DEAD = -2, NEW = -1, FREE = 0, PARTIAL = 1, FULL = 2, CACHE = 3
};

static inline void update_slab_state_after_alloc(struct kmem_cache *cache, struct slab *slab, enum STATE oldstate);
static inline void update_slab_state_after_free(struct kmem_cache *cache, struct slab *slab, enum STATE oldstate);
static inline struct slab *slab_create(uint object_size);
static inline void slab_destroy(struct slab *oldslab);
static inline struct run *slab_alloc(struct slab *slab, uint object_size);
static inline void slab_free(struct slab *slab, struct run *obj, uint object_size);
static inline struct run *freelist_end_append(struct run *freelist, struct run *obj, uint object_size);

#define MAX_SPACE(type)             (PGSIZE - sizeof(type))
#define MAX_OBJS(type, object_size) (MAX_SPACE(type) / object_size)
// object address computation (type: char*)
#define OBJ_LAST_OFFSET(header_type, object_size)  (sizeof(header_type) + (MAX_OBJS(header_type, object_size) - 1) * object_size)
#define OBJ_START(header_type, header)             ((char*)(header) + sizeof(header_type))
#define OBJ_LAST(header_type, header, object_size) ((char*)(header) + OBJ_LAST_OFFSET(header_type, object_size))
#define OBJ_END(header)                            ((char*)(header) + PGSIZE)
#define OBJ_NEXT(object_addr, object_size)         ((char*)(object_addr) + (object_size))
// accessing free list addresses (type: struct run)
#define GET_FREELIST_START(s)         (((s)->freelist_start) ? ((struct run*)((char*)(s) + (s)->freelist_start)) : NULL)
#define SET_FREELIST_START(s, objrun) ((s)->freelist_start = ((objrun) ? ((unsigned)(((char*)(objrun)) - ((char*)(s)))) : 0))
#define GET_FREELIST_END(s)           (((s)->freelist_end) ? ((struct run*)((char*)(s) + (s)->freelist_end)) : NULL)
#define SET_FREELIST_END(s, objrun)   ((s)->freelist_end = ((objrun) ? ((unsigned)(((char*)(objrun)) - ((char*)(s)))) : 0))
// contiguous object traversal
#define OBJ_FOR_EACH(objrun_type, objrun, header_type, header, object_size) \
        for(objrun = (objrun_type*)(OBJ_START(header_type, header)); \
            OBJ_NEXT(objrun, object_size) <= OBJ_END(header); \
            objrun = (objrun_type*)(OBJ_NEXT(objrun, object_size)))

void print_kmem_cache(struct kmem_cache *cache, void (*slab_obj_printer)(void *))
{
  acquire(&cache->lock);
  debug("[SLAB] kmem_cache { name: %s, object_size: %u, at: %p, in_cache_obj: %lu }\n", cache->name, cache->object_size, cache, MAX_OBJS(struct kmem_cache, cache->object_size));
  // print the info of the type "cache" slab, i.e., kmem_cache as a slab.
  debug("[SLAB]    [ cache slabs ]\n[SLAB]        [ slab %p ] { freelist: %p, nxt: %p }\n", cache, GET_FREELIST_START(cache), NULL); // nxt does not mean anything here
  struct run *obj;
  uint idx = 0;
  OBJ_FOR_EACH(struct run, obj, struct kmem_cache, cache, cache->object_size){
    if(cache->lazy_list_enabled && obj >= GET_FREELIST_START(cache) && obj < (struct run*)OBJ_LAST(struct kmem_cache, cache, cache->object_size)){
      obj->next = (struct run*)OBJ_NEXT(obj, cache->object_size);
      if(OBJ_NEXT(obj, cache->object_size) == OBJ_LAST(struct kmem_cache, cache, cache->object_size))
        cache->lazy_list_enabled = 0;
    }
    debug("[SLAB]           [ idx %u ] { addr: %p, as_ptr: %p, as_obj: {", idx, obj, obj->next);
    slab_obj_printer((void*)obj);
    debug("%s", "} }\n");
    idx++;
  }
  cache->lazy_list_enabled = 0;
  // print the info of all type "partial" slabs.
  struct list_head *node;
  list_for_each(node, &cache->partial){
    struct slab *entry = list_entry(node, struct slab, link);
    debug("[SLAB]    [ partial slabs ]\n[SLAB]        [ slab %p ] { freelist: %p, nxt: %p }\n", entry, GET_FREELIST_START(entry), entry->link.next);
    idx = 0;
    OBJ_FOR_EACH(struct run, obj, struct slab, entry, cache->object_size){
      if(entry->lazy_list_enabled && obj >= GET_FREELIST_START(entry) && obj < (struct run*)OBJ_LAST(struct slab, entry, cache->object_size)){
        obj->next = (struct run*)OBJ_NEXT(obj, cache->object_size);
        if(OBJ_NEXT(obj, cache->object_size) == OBJ_LAST(struct slab, entry, cache->object_size))
          entry->lazy_list_enabled = 0;
      }
      debug("[SLAB]           [ idx %u ] { addr: %p, as_ptr: %p, as_obj: {", idx, obj, ((struct run*)obj)->next);
      slab_obj_printer((void*)obj);
      debug("%s", "} }\n");
      idx++;
    }
  }
  // as the spec requested, we do not print the "full" or "free" slabs even if we maintain lists for them.
  debug("%s", "[SLAB] print_kmem_cache end\n");
  release(&cache->lock);
}

struct kmem_cache *kmem_cache_create(char *name, uint object_size)
{
  if(object_size < sizeof(struct run) || object_size > MAX_SPACE(struct slab)){
    debug("[slab] kmem_cache_create: object size %u is too small or too big, must be with %lu and %lu to create cache %s\n", object_size, sizeof(struct run), MAX_SPACE(struct slab), name);
    return NULL;
  }
  // allocate a page
  struct kmem_cache *cache = (struct kmem_cache*)kalloc();
  if(!cache){
    debug("[slab] kmem_cache_create: kalloc failed for cache %s\n", name);
    return NULL;
  }
  // initialize
  safestrcpy(cache->name, name, sizeof(cache->name)); // size includes NULL, so we input "sizeof(cache->name)" but not "sizeof(cache->name)-1"
  cache->object_size = object_size;
  initlock(&cache->lock, "kmem_cache");
  INIT_LIST_HEAD(&cache->full);
  INIT_LIST_HEAD(&cache->partial);
  INIT_LIST_HEAD(&cache->free);
  cache->num_avail_slab = 0;
  // make a freelist for "kmem_cache as a slab", i.e., to utilize the rest of the page since we call kalloc for only a small struct kmem_cache, we make kmem_cache a special slab. we say it is of type "cache" (which does not belong to full/partial/free)
  // originally we wrote SLAB_INIT_FREELIST(struct kmem_cache, cache, object_size);  if(!cache->freelist_start) { kfree((void*)cache); return NULL; }
  // but to achieve true O(1), we adopt lazy list initialization instead.
  cache->freelist_start = sizeof(struct kmem_cache);                            debug("%u\n", cache->freelist_start);
  struct run *first = GET_FREELIST_START(cache);
  first->next = (struct run*)(OBJ_NEXT(first, cache->object_size));             debug("%p\n", first->next);
  cache->freelist_end = OBJ_LAST_OFFSET(struct kmem_cache, object_size);        debug("%u\n", cache->freelist_end);
  struct run *last = GET_FREELIST_END(cache);
  last->next = NULL;                                                            debug("%p\n", last->next);
  cache->lazy_list_enabled = 1;
  // print info
  debug("[SLAB] New kmem_cache (name: %s, object size: %u bytes, at: %p, max objects per slab: %lu, support in cache obj: %lu) is created\n", name, object_size, cache, MAX_OBJS(struct slab, object_size), MAX_OBJS(struct kmem_cache, object_size));
  return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
  debug("[slab] kmem_cache_destroy: destroying kmem_cache %p\n", cache);
  // free every single page kalloc for all the slabs in the slab lists kmem_cache manages
  struct list_head *head[3] = {&cache->full, &cache->partial, &cache->free}; // we don't not need this because the three list_head full, partial, free are consecutive members in struct kmem_cache, but we want to be safer
  int i;
  for(i = 0; i < 3; i++){
    struct list_head *node, *safe;
    list_for_each_safe(node, safe, head[i]){ // here we cannot use list_for_each_entry_safe because of compiler issues
      slab_destroy(list_entry(node, struct slab, link));
    }
  }
  // free the page kalloc for kmem_cache itself
  list_del_init(&cache->partial); // this is kinda redundant since all slabs are destroyed
  kfree((void*)cache);
}

void *kmem_cache_alloc(struct kmem_cache *cache)
{
  acquire(&cache->lock); // acquire the lock before modification
  debug("[SLAB] Alloc request on cache %s\n", cache->name);
  struct run *obj;
  // [CACHE] is the "cache" type slab, i.e., kmem_cache as a slab, not full yet?
  if(cache->freelist_start){
    // allocate one object to "kmem_cache as a slab"
    obj = GET_FREELIST_START(cache);
    if(cache->lazy_list_enabled){
      SET_FREELIST_START(cache, (struct run*)OBJ_NEXT(obj, cache->object_size));
      if(cache->freelist_start >= OBJ_LAST_OFFSET(struct kmem_cache, cache->object_size)){  // which means we cannot store an object at cache->freelist_start + cache->object_size
        cache->lazy_list_enabled = 0;
      } else{
        struct run *next = GET_FREELIST_START(cache);
        next->next = (struct run*)(OBJ_NEXT(next, cache->object_size));
      }
    } else{
      SET_FREELIST_START(cache, obj->next);
    }
    // handling the last allocated object (then the list goes from nonempty to empty)
    if(!cache->freelist_start){
      cache->freelist_end = 0;
    }
    debug("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", obj, cache, cache->name);
    // "kmem_cache as a slab" is always in "cache" type, i.e., no state changes within "full/partial/free"
    release(&cache->lock); // release the lock before return
    return (void*)obj;
  }
  // "kmem_cache as a slab" is full. check the others.
  struct slab *slab;
  enum STATE oldstate; 
  if(!list_empty(&cache->partial)){    // [PARTIAL] does a "partial" slab exist?
    slab = list_first_entry(&cache->partial, struct slab, link);
    oldstate = PARTIAL;
  }else if(!list_empty(&cache->free)){ // [FREE] does a "free" slab exist?
    slab = list_first_entry(&cache->free, struct slab, link);
    oldstate = FREE;
  }else{                               // [NEW] "kmem_cache as a slab" is full and no "partial/free" slabs, i.e., all slabs are full. create a new slab.
    if(!(slab = slab_create(cache->object_size))){ // the slab is not linked to any list here.
      debug("[slab] kmem_cache_alloc: failed to allocate a new slab for cache %s\n", cache->name);
      release(&cache->lock); // release the lock before return
      return NULL;
    }
    debug("[SLAB] A new slab %p (%s) is allocated\n", slab, cache->name);
    oldstate = NEW;
  }
  // allocate one object to the slab
  if(!(obj = slab_alloc(slab, cache->object_size))){ // the "if" theoretically never happens since "slab->freelist_start == 0" means "full"
    debug("[slab] kmem_cache_alloc: attempted to allocate from a 'full' partial/free/new slab ('full' because 'slab->freelist_start == 0') in cache %s\n", cache->name);
    release(&cache->lock); // release the lock before return
    return NULL;
  }
  debug("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", obj, slab, cache->name);
  update_slab_state_after_alloc(cache, slab, oldstate); // update the slab state
  release(&cache->lock); // release the lock before return
  return (void*)obj;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
  acquire(&cache->lock); // acquire the lock before modification
  uint64 slab_type = PGROUNDDOWN((uint64)obj);
  // [CACHE] obj is in the slab of "cache" type, i.e., kmem_cache as a slab.
  if(slab_type == (uint64)cache){
    // free the object
    SET_FREELIST_END(cache, freelist_end_append(GET_FREELIST_END(cache), obj, cache->object_size));
    if(!cache->freelist_start){
      cache->freelist_start = cache->freelist_end;
    }
    debug("[SLAB] Free %p in slab %p (%s)\n[SLAB] End of free\n", obj, cache, cache->name);
    //printf("freelist_end at free: %p\n", GET_FREELIST_END(cache));
    //printf("freelist_end->next: %p\n", GET_FREELIST_END(cache)->next);
    release(&cache->lock); // release the lock before return
    return;
  }
  // [PARTIAL/FULL] obj is in a slab of type "full" or "partial" (you cannot free an object from a free slab)
  struct slab *slab = (struct slab*)slab_type;
  enum STATE oldstate = (slab->freelist_start) ? PARTIAL : FULL;
  // Free the object.
  slab_free(slab, obj, cache->object_size);
  debug("[SLAB] Free %p in slab %p (%s)\n", obj, slab, cache->name);
  update_slab_state_after_free(cache, slab, oldstate);
  debug("%s", "[SLAB] End of free\n");
  release(&cache->lock); // release the lock before return
}


// custom functions

static inline void update_slab_state_after_alloc(struct kmem_cache *cache, struct slab *slab, enum STATE oldstate){
  // update the slab state from {new, free, partial} to {partial, full}, denoted ([from],[to]). other transitions are impossible for an allocation.
  // enum STATE newstate = (!slab->freelist_start) ? FULL : PARTIAL;
  if(!slab->freelist_start){
    list_move(&slab->link, &cache->full);       // (new/free/partial,full) ~> move to full
    if(oldstate == FREE || oldstate == PARTIAL)
      cache->num_avail_slab--;                  // (free,full) or (partial,full) ~> from avail to not avail
  }else{
    if(oldstate == NEW || oldstate == FREE){
      list_move(&slab->link, &cache->partial);  // (new/free,partial) ~> move to partial
    }
    if(oldstate == NEW)
      cache->num_avail_slab++;                  // (new,partial) ~> from not avail to avail
                                                // (partial,partial) ~> no state changes
  }
}

static inline void update_slab_state_after_free(struct kmem_cache *cache, struct slab *slab, enum STATE oldstate){
  // update the slab state from {partial, full} to {dead, free, partial}, denoted ([from],[to]). other transitions are impossible for a freeing.
  // enum STATE newstate = ...? (here we note that when avail slabs are enough, i.e., + this one > MIN_AVAIL_SLAB, kfree more aggressively)
  if(!slab->num_objs_in_use){
    if(cache->num_avail_slab + 1 > MP2_MIN_AVAIL_SLAB){       // DEAD;
      slab_destroy(slab);                       // (partial/full,dead) ~> slab_destroy, i.e., kalloc() page freed
      debug("[SLAB] slab %p (%s) is freed due to save memory\n", slab, cache->name);
      if(oldstate == PARTIAL)
        cache->num_avail_slab--;                // (partial,dead) ~> from avail to not avail
    }else{                                                    // FREE;
      list_move(&slab->link, &cache->free);     // (partial/full,free) ~> move to free
      if(oldstate == FULL)
        cache->num_avail_slab++;                // (full,free) ~> from not avail to avail
    }
  }else{                                                      // PARTIAL;
    if(oldstate == FULL){
      list_move(&slab->link, &cache->partial);  // (full, partial) ~> move to partial
      cache->num_avail_slab++;                  // (full, partial) ~> from not avail to avail
    }
                                                // (partial,partial) ~> no state changes
  }
}

static inline struct slab *slab_create(uint object_size){
  if (object_size < sizeof(struct run) || object_size > MAX_SPACE(struct slab)) {
    debug("[slab] slab_create: object size %u is too small or too big, must be with %lu and %lu to create a slab\n", object_size, sizeof(struct run), MAX_SPACE(struct slab));
    return NULL;
  }
  struct slab *newslab = (struct slab*)kalloc();
  if(!newslab){
    debug("%s", "[slab] slab_create: kalloc failed\n");
    return NULL;
  }
  INIT_LIST_HEAD(&newslab->link);
  // originally we wrote SLAB_INIT_FREELIST(struct slab, newslab, object_size);  if(!newslab->freelist_start) { kfree((void*)newslab); return NULL; }
  // but to achieve true O(1), we adopt lazy list initialization instead.
  newslab->freelist_start = sizeof(struct slab);                            debug("%u\n", newslab->freelist_start);
  struct run *first = GET_FREELIST_START(newslab);
  first->next = (struct run*)(OBJ_NEXT(first, object_size));                debug("%p\n", first->next);
  newslab->freelist_end = OBJ_LAST_OFFSET(struct slab, object_size);        debug("%u\n", newslab->freelist_end);
  struct run *last = GET_FREELIST_END(newslab);
  last->next = NULL;                                                        debug("%p\n", last->next);
  newslab->lazy_list_enabled = 1;
  newslab->num_objs_in_use = 0;
  debug("[slab] slab_create: new slab (object size: %u bytes, at: %p) is created\n", object_size, newslab);
  return newslab;
}

static inline void slab_destroy(struct slab *oldslab){
  debug("[slab] slab_destroy: destroying slab %p\n", oldslab);
  list_del_init(&oldslab->link);
  kfree((void*)oldslab);
}

static inline struct run *slab_alloc(struct slab *slab, uint object_size){
  if(!slab->freelist_start){
    debug("[slab] slab_alloc: attempted to allocate from a full slab ('full' because 'slab->freelist_start == 0')\n");
    return NULL;
  }
  struct run *obj = GET_FREELIST_START(slab);
  if(slab->lazy_list_enabled){
    SET_FREELIST_START(slab, (struct run*)OBJ_NEXT(obj, object_size));
    if(slab->freelist_start >= OBJ_LAST_OFFSET(struct slab, object_size)){  // which means we cannot store an object at slab->freelist_start + object_size
      slab->lazy_list_enabled = 0;
    }
    else{
      // prepare the next in free range
      struct run *next = GET_FREELIST_START(slab);
      next->next = (struct run*)(OBJ_NEXT(next, object_size));
    }
  }
  else{
    SET_FREELIST_START(slab, obj->next);
  }
  // handling the last allocated object (then the list goes from nonempty to empty)
  if(!slab->freelist_start){
    slab->freelist_end = 0;
  }
  slab->num_objs_in_use++;
  return obj;
}

static inline void slab_free(struct slab *slab, struct run *obj, uint object_size){
  SET_FREELIST_END(slab, freelist_end_append(GET_FREELIST_END(slab), obj, object_size));
  // handling the first freed object (then the list goes from empty to nonempty)
  if(!slab->freelist_start){
    slab->freelist_start = slab->freelist_end;
  }
  //printf("freelist_end at free: %p\n", GET_FREELIST_END(slab));
  //printf("freelist_end->next: %p\n", GET_FREELIST_END(slab)->next);
  slab->num_objs_in_use--;
}

static inline struct run *freelist_end_append(struct run *freelist, struct run *obj, uint object_size){
  // i cannot fill with junk, i.e., do memset(obj, 1, object_size);, to catch dangling refs here because, by te spec, print_kmem_cache might be interested in the freed objects
  // we use FIFO. (it was LIFO: struct run *r = (struct run*)obj; r->next = freelist; freelist = r; return freelist;)

  memset(obj, 1, object_size);
  struct run *r = (struct run*)obj;
  r->next = NULL;
  if(freelist == NULL){
    freelist = r;
  }
  else{
    freelist->next = r;
  }
  return r; // update freelist_end to r
}

/* OLD METHOD: DILIGENT and LIFO (now it is LAZY and FIFO)

#define SLAB_INIT_FREELIST(header_type, header, obj_size) \
    { \
        (header)->freelist_start = 0; \
        SET_FREELIST_START(header, freelist_freerange(GET_FREELIST_START(header), (void*)OBJ_START(header_type, header), (void*)OBJ_END(header), obj_size)); \
    }

struct run *freelist_freerange(struct run *freelist, void *obj_start, void *obj_end, uint object_size){
  if(object_size == 0){
    debug("%s", "[slab] freelist_freerange: object_size should not be zero\n");
    return freelist;
  }
  char *obj = (char*)obj_start;
  if(obj + object_size > (char*)obj_end){
    debug("%s", "[slab] freelist_freerange: obj_start should be a smaller address than obj_end\n");
    return freelist;
  }
  for(; obj + object_size <= (char*)obj_end; obj += object_size){
    struct run *r = (struct run*)obj;
    r->next = freelist;
    freelist = r;
  }
  return freelist;
}

*/