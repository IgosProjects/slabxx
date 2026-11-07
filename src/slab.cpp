/*
    This file is licensed under the MIT license!
    DESC: malloc test impl, use this as like a tutorial on how to make a malloc in your kernel
*/

#include <stdint.h>
#include <stdarg.h>

/* headers to only import when NOT building for freestanding */
/* you wont need these anyway since you provide ur own "kernel_*" functions! */
#ifdef __STDC_HOSTED__
#include <malloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#endif

// NOTICE: this file is a fixed up version of https://github.com/Dcraftbg/Slab/blob/2b087b399e6d24a4585b6e753cdad73b43b41e06/src/main.c
// it also includes fixes for C++ since stuff like "void*" arithmetic dont work here!

/* KERNEL API START */

/*
    NOTE: swap with your own page allocating impl when porting to ur OS
*/
void* alloc_page(size_t pages) {
    return malloc(4096 * pages);
}

/*
    NOTE: pages is unused by defualt! if your kernel needs it, youc an use it!
*/
void free_page(void* ptr, size_t pages) {
    free(ptr);
}

void* kernel_malloc(size_t memory) {
    return alloc_page((memory+4095)/4096);
}

void kernel_free(void* data, size_t size) {
    return free_page(data, (size+4095)/4096);
}

/*
    Errors out if the provided condition ends up as FALSE, otherwise it will return!
    NOTE: its a macro because THAT was easier!
*/
#define kernel_assert(a) (assert(a))

/*
    PrintF-like interface that logs to the kernel's console
*/
void kernel_log(const char* s, ...) {
    /* NOTE: this is a really hacky way, but you gotta what your gotta do! */
    va_list args;
    va_start(args, s);

    vprintf(s, args);

    va_end(args);
}

/*
    Sets N bytes from S into C
*/
void *kernel_memset(void *__s, int __c, size_t __n) {
    return memset(__s, __c, __n); // this is just a fancy stack smasher aint it?
}

/* KERNEL API END */

#define PAGE_ALIGN(x) ((((x)+4095)/4096) * 4096)

/* 
 * NOTE: the below implementation is a slightly modifed version of the Linux one: 
 * https://github.com/torvalds/linux/blob/b1bc554e009e3aeed7e4cfd2e717c7a34a98c683/tools/firewire/list.h
 * 
 * things like list_insert and list_append are all the same except for list_remove which also closes the 
 * array element since the Linux version didn't!
*/
struct list {
    struct list *next, *prev;
};

// Gets the LIST ready for operation
static void list_init(struct list* list) {
    list->next = list;
    list->prev = list;
}

// returns the last item in the list
static struct list* list_last(struct list* list) {
    return list->prev != list ? list->prev : nullptr;
}

// returns the next item in the list
static struct list* list_next(struct list* list) {
    return list->next != list ? list->next : nullptr;
}

// inserts a new entry into the list
static inline void list_insert(struct list *new_l, struct list *link) {
    new_l->prev = link->prev;
    new_l->next = link;
    new_l->prev->next = new_l;
    new_l->next->prev = new_l;
}

// appends a new entry to the provided list
static void list_append(struct list *new_l, struct list *into) {
    list_insert(new_l, into);
}

// removes the list? idk
static void list_remove(struct list* list) {
    list->prev->next = list->next;
    list->next->prev = list->prev;
    list->next = list;
    list->prev = list;
}

struct slab; // predefine the slab struct

// single slab cache struct
typedef struct Cache {
    // linker list to:
    // - fully filled blocks     => blocks with no available memory
    // - partially filled blocks => blocks with partially available memory
    // - free blocks             => empty blocks with fully available memory
    struct list full, partial, free;

    // total amount of objects in cache
    size_t totalobjs;

    // amount of objects in cache
    size_t inuse;

    // size of each object
    size_t objsize;
    
    // amoutn of objects PER slab
    size_t objs_per_slab;
};

typedef struct Slab { 
    struct list list;
    Cache* cache; // Cache it belongs to
    void* memory;
    // maitain a struct of free objects
    size_t free; 
};

// cache for caches? funny i guess?
Cache* cache_cache = nullptr;

// skip the slab's data(+1, evuivilent to +sizeof(Slab))
// and right after it you will find an array of bufctl aka
// the stack of free objects
#define slab_bufctl(slab) ((uint32_t*)(slab + 1))

static size_t slab_mem(Cache* cache) {
    return cache->objs_per_slab * (cache->objsize + sizeof(uint32_t)) + sizeof(Slab);
}

// grows the provided cache by one entry
intptr_t cache_grow(Cache* cache) {
    void *addr = kernel_malloc(slab_mem(cache));
    if(!addr) return -1; // @STATUS 
    Slab* slab = (Slab*)addr;
    slab->cache = cache;
    // memory follows right after cache
    slab->memory = ((uint32_t*)((uint8_t*)addr + sizeof(*slab))) + cache->objs_per_slab;
    slab->free = 0;
    cache->totalobjs+=cache->objs_per_slab;
    for(size_t i = 0; i < cache->objs_per_slab-1; ++i) {
        slab_bufctl(slab)[i] = i;
    }
    list_append((struct list*)slab, &cache->free);
    return 0;
}

// selects a cache where you can allocate your objects
static Slab* cache_select(Cache* cache) {
    Slab* s = nullptr;
    if((s = (Slab*)list_next(&cache->partial))) return s;
    if((s = (Slab*)list_next(&cache->free))) return s;
    if(cache_grow(cache) != 0) return nullptr; // @STATUS
    s = (Slab*)list_next(&cache->free);
    return s;
}

// allocates a slab insdie of the cache
void* cache_alloc(Cache* cache) {
    Slab* slab = cache_select(cache);
    if(!slab) return NULL;
    size_t index = slab_bufctl(slab)[slab->free];
    void* ptr = (uint8_t*)slab->memory + cache->objsize * index;

    // the slab was free before, lets move it to partial!
    if(slab->free++ == 0) {
        list_remove(&slab->list);
        list_append(&slab->list, &cache->partial);
    }

    // the slab was paritially filled but now its filled, lets move it to full!
    if(slab->free == cache->objs_per_slab) {
        list_remove(&slab->list);
        list_append(&cache->full, &slab->list);
    }
    cache->inuse++;
    return ptr;
}

// converts a slab pointer into an index
// C++ makes the need for many conversions actually exist
static size_t slab_ptr_to_index(Cache* cache, Slab* slab, void* p) {
    return (size_t)((uintptr_t)p - (uintptr_t)slab->memory) / cache->objsize;
}

static bool cache_dealloc_within(Cache* cache, Slab* slab, void* p) {
    Slab *first = slab;
    while(slab != (Slab*)first->list.prev) {
        uint8_t* base = (uint8_t*)slab->memory;
        if (p >= (void*)base && p < (void*)(base + cache->objsize * cache->objs_per_slab)) {
            assert(slab->free);
            if(slab->free-- == cache->objs_per_slab) {
                list_remove(&slab->list);
                list_append(&cache->partial, &slab->list);
            }
            if(slab->free == 0) {
                list_remove(&slab->list);
                list_append(&cache->free, &slab->list);
            }
            size_t index = slab_ptr_to_index(cache, slab, p);
            slab_bufctl(slab)[slab->free]=index;
            cache->inuse--;
            return true;
        }
        slab = (Slab*)slab->list.next;
    }
    return false;
}

// free's the provided memory address in cache
void cache_free(Cache* cache, void* p) {
    Slab *s = (Slab*)list_next(&cache->full);
    if(s) {
        if(cache_dealloc_within(cache, s, p)) return;
    }
    s = (Slab*)list_next(&cache->partial);
    if(s) {
        if(cache_dealloc_within(cache, s, p)) return;
    }
}

// gets the provided cache object to work
void init_cache(Cache* c, size_t objsize) {
    kernel_memset(c, 0, sizeof(*c)); 
    list_init(&c->partial);
    list_init(&c->full);
    list_init(&c->free);
    c->objsize = objsize;
    // Gready algorithmn
    c->objs_per_slab = PAGE_ALIGN(objsize) / objsize;
}

// creates a NEW cache, automatically calls "init_cache"
Cache* create_new_cache(size_t objsize) {
    Cache* c = (Cache*)cache_alloc(cache_cache);
    if(!c) return NULL;
    init_cache(c, objsize);
    return c;
}

struct Foo {
    uintptr_t a;
    uintptr_t b;
};

// @DEBUG
static void log_list(struct list* list) {
    struct list* first = list;
    list = list->next; // Always skip the first one in this case, since cache.full/partial/free is not a valid Slab
    while(first != list) {
        printf("- %p\n",list);
        list = list->next;
    }
}
// @DEBUG
static void log_cache(Cache* cache) {
    kernel_log("Cache:\n");
    kernel_log("Objects: %zu/%zu\n",cache->inuse,cache->totalobjs);
    kernel_log("Object Size: %zu\n",cache->objsize);
    kernel_log("Object Per Slab: %zu\n",cache->objs_per_slab);
    kernel_log("Full:\n");
    log_list(&cache->full);
    kernel_log("Partial:\n");
    log_list(&cache->partial);
    kernel_log("Free:\n");
    log_list(&cache->free);
}

int main(void) {
    cache_cache = (Cache*)kernel_malloc(sizeof(Cache));
    init_cache(cache_cache, sizeof(Cache));
    kernel_log("cache_cache: %p", cache_cache);
    Cache* foo_cache = NULL;
    void* p = NULL;
    foo_cache = create_new_cache(sizeof(Foo));

    kernel_log("Foo cache: %p\n",foo_cache);
    p = cache_alloc(foo_cache);
    kernel_log("Foo: %p\n",p);
#if 1
    kernel_log("deallocating...\n");
    cache_free(foo_cache, p);
#else
    kernel_log("NOT deallocating...\n");
#endif
    p = cache_alloc(foo_cache);
    kernel_log("Foo: %p\n",p);
    log_cache(foo_cache);
}
