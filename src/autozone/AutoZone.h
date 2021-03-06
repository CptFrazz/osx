/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */
/*
    AutoZone.h
    Copyright (c) 2004-2009 Apple Inc. All rights reserved.
 */

#pragma once
#ifndef __AUTO_ZONE_CORE__
#define __AUTO_ZONE_CORE__

#include "auto_zone.h"
#include "auto_impl_utilities.h"
#include "auto_weak.h"

#include "AutoBitmap.h"
#include "AutoConfiguration.h"
#include "AutoDefs.h"
#include "AutoLarge.h"
#include "AutoLock.h"
#include "AutoAdmin.h"
#include "AutoRegion.h"
#include "AutoStatistics.h"
#include "AutoSubzone.h"
#include "AutoThread.h"

#include <algorithm>
#include <cassert>

#define USE_DISPATCH_QUEUE 1

#if USE_DISPATCH_QUEUE
#include <dispatch/dispatch.h>
#endif

namespace Auto {

    //
    // Forward declarations.
    //
    class Monitor;
    
    //
    // TrackedPageAllocator -- Tracks the number of pages in use in a PointerArray by calling Statistics::add_admin() with all
    // requested allocation and deallocation sizes.
    //
    class TrackedPageAllocator {
    private:
        Statistics                     &_stats;

    public:
        TrackedPageAllocator(Statistics &st) : _stats(st) {}
        TrackedPageAllocator(const TrackedPageAllocator &allocator) : _stats(allocator._stats) {}
    
        inline void *allocate_memory(usword_t size) {
            _stats.add_admin(size);
            return Auto::allocate_memory(size);
        }
        
        inline void deallocate_memory(void *address, usword_t size) {
            _stats.add_admin(-(intptr_t)size);
            Auto::deallocate_memory(address, size);
        }
        
        inline void uncommit_memory(void *address, usword_t size) {
            Auto::uncommit_memory(address, size);
        }
        
        inline void copy_memory(void *dest, void *source, usword_t size) {
            Auto::copy_memory(dest, source, size);
        }
    };
    typedef PointerArray<TrackedPageAllocator> PointerList;

    template <typename BarrierType>
    class EnliveningHelper : public BarrierType {
        Thread &_thread;
    public:
        EnliveningHelper(Thread &thread) : BarrierType(thread.enlivening_queue().needs_enlivening()), _thread(thread) {}
        void enliven_block(void *block) { _thread.enlivening_queue().add(block); }
    };
    
     //----- ScanStack -----//
    
    class ScanStack {
      
      private:
        void **                _address;                    // base address of stack
        void **                _end;                        // stack last + 1
        void **                _cursor;                     // top of stack + 1
        void **                _highwater;                  // largest stack required
      
      public:
      
        //
        // Constructor
        //
        ScanStack()
        : _address(NULL)
        , _end(NULL)
        , _cursor(NULL)
        , _highwater(NULL)
        {}
        
        
        //
        // set_range
        //
        // Set the stack base address and end.
        //
        inline void set_range(Range range) {
            set_range(range.address(), range.end());
        }
        inline void set_range(void *address, void *end) {
            _address = (void **)address;
            _end = (void **)end;
            _cursor = (void **)address;
            _highwater = (void **)address;
        }
        inline void set_range(void *address, usword_t size) {
            set_range(address, displace(address, size));
        }
        
        
        //
        // reset
        //
        // Resets the stack back to initial state.
        //
        void reset() {
            _cursor = _address;
            _highwater = _address;
        }
        

        //
        // is_allocated
        //
        // Returns true if a stack has been allocated.
        //
        inline bool is_allocated() const {
            return _address != NULL;
        }
        

        //
        // is_empty
        //
        // Returns true if a stack contains no elements.
        //
        inline bool is_empty() const {
            return _cursor == _address;
        }
        

        //
        // is_overflow
        //
        // Returns true if a stack overflow condition has occurred.
        //
        inline bool is_overflow() const {
            return _cursor == _end;
        }
        
        

        //
        // push
        //
        // Push the block onto the scan stack.
        //
        inline void push(void *block) {
            // if overflow then lock at overflow
            if (!is_overflow()) {
                *_cursor++ = block;
                if (_highwater < _cursor) _highwater = _cursor;
            }
        }
        
        
        //
        // top
        //
        // Return the top block in the stack.
        //
        inline void *top() {
            // If non empty return TOS (if overflow then lock at overflow)
            if (!is_empty() && !is_overflow()) return _cursor[-1];
            return NULL;
        }
        
        
        //
        // pop
        //
        // Return the next block to scan.
        //
        inline void *pop() {
            // If non empty return TOS (if overflow then lock at overflow)
            if (!is_empty() && !is_overflow()) return *--_cursor;
            return NULL;
        }
        
        //
        // max items
        //
        // return the maximum number of items on the stack since last reset
        //
        inline unsigned long max() {
            return _highwater - _address;
        }
    };

    typedef std::vector<Range, AuxAllocator<Range> > RangeVector;
    class ObjectAssocationHashMap : public PtrPtrMap, public AuxAllocated {}; // <rdar://problem/7217591> Reduce space usage for each association.
    typedef __gnu_cxx::hash_map<void *, ObjectAssocationHashMap*, AuxPointerHash, AuxPointerEqual, AuxAllocator<void *> > AssocationsHashMap;

    //----- Zone -----//
    
    enum State {
        idle, scanning, enlivening, finalizing, reclaiming
    };

    class Zone {
    friend class MemoryScanner;
        
#define INVALID_THREAD_KEY_VALUE ((Thread *)-1)
        
      public:

        malloc_zone_t         basic_zone;

        bool                  multithreaded;                      // if set, will run collector on dedicated thread
      
        volatile int32_t      collector_disable_count;            // counter for external disable-collector API
        volatile int32_t      collector_collecting;               // internal guard to prevent recursion and/or several threads attempting gc at once
        uint32_t              collection_count;


        // collection control
        auto_collection_control_t       control;

        // statistics
        auto_lock_t           stats_lock;               // only affects fields below; only a write lock; read access may not be accurate, as we lock statistics independently of the main data structures
        auto_statistics_t     stats;
    
        // weak references
        unsigned              num_weak_refs;
        unsigned              max_weak_refs;
        struct weak_entry_t  *weak_refs_table;
        spin_lock_t           weak_refs_table_lock;

#if USE_DISPATCH_QUEUE
        dispatch_queue_t      collection_queue;
        dispatch_block_t      collection_block;
#else
        pthread_t             collection_thread;
        pthread_cond_t        collection_requested;
#endif
        pthread_mutex_t       collection_mutex;
        volatile uint32_t     collection_requested_mode;
        pthread_cond_t        collection_status;
        volatile uint32_t     collection_status_state;

      private:
      
        //
        // Shared information
        //
        // watch out for static initialization
        static volatile int32_t _zone_count;                // used to generate _zone_id
        static Zone           *_first_zone;                 // for debugging
        
        //
        // thread management
        //
        Thread                *_registered_threads;         // linked list of registered threads
        pthread_key_t          _registered_threads_key;     // pthread key for looking up Thread instance for this zone
        pthread_mutex_t        _registered_threads_mutex;   // protects _registered_threads and _enlivening_enabled
        bool                   _enlivening_enabled;         // tracks whether new threads should be initialized with enlivening on
        bool                   _enlivening_complete;        // tracks whether or not enlivening has been performed on this collection cycle.
        
        pthread_mutex_t       _mark_bits_mutex;             // protects the per-Region and Large block mark bits.

        //
        // memory management
        //
        Bitmap                 _in_subzone;                 // indicates which allocations are used for subzone region
        Bitmap                 _in_large;                   // indicates which allocations are used for large blocks
        Large                 *_large_list;                 // doubly linked list of large allocations
        spin_lock_t            _large_lock;                 // protects _large_list, _in_large, and large block refcounts
        PtrHashSet             _roots;                      // hash set of registered roots (globals)
        spin_lock_t            _roots_lock;                 // protects _roots
        RangeVector            _datasegments;               // registered data segments.
        spin_lock_t            _datasegments_lock;          // protects _datasegments
        PtrHashSet             _zombies;                    // hash set of zombies
        spin_lock_t            _zombies_lock;               // protects _zombies
        Region                *_region_list;                // singly linked list of subzone regions
        spin_lock_t            _region_lock;                // protects _region_list
        PtrIntHashMap          _retains;                    // STL hash map of retain counts
        spin_lock_t            _retains_lock;               // protects _retains
        bool                   _repair_write_barrier;       // true if write barrier needs to be repaired after full collection.
        ScanStack              _scan_stack;                 // stack used suring scanning
        Range                  _coverage;                   // range of managed memory
        spin_lock_t            _coverage_lock;              // protects _coverage
        TrackedPageAllocator   _page_allocator;             // Statistics tracking page allocator.
        Statistics             _stats;                      // statistics for this zone
        int32_t                _allocation_threshold;       // byte allocation counter (reset after each collection).
        PointerList            _garbage_list;               // vm_map allocated pages to hold the garbage list.
        AssocationsHashMap     _associations;               // associative references object -> ObjectAssocationHashMap*.
        spin_lock_t            _associations_lock;          // protects _associations
        bool                   _scanning_associations;      // tells 
        volatile enum State    _state;                      // the state of the collector
        
#if UseArena
        void                    *_arena;                    // the actual 32G space (region low, larges high)
        void                    *_large_start;              // half-way into arena + size of bitmaps needed for region
        Bitmap                  _large_bits;                // bitmap of top half - tracks quanta used for large blocks
        spin_lock_t             _large_bits_lock;           // protects _large_bits
#endif
        Admin                   _small_admin;               // small quantum admin
        Admin                   _medium_admin;              // medium quantum admin
        
        //
        // thread safe Large deallocation routines.
        //
        void deallocate_large(void *block);
        
        //
        // allocate_region
        //
        // Allocate and initialize a new subzone region.
        //
        Region *allocate_region();
        
        
        //
        // allocate_large
        //
        // Allocates a large block from the universal pool (directly from vm_memory.)
        //
        void *allocate_large(Thread &thread, usword_t &size, const unsigned layout, bool clear, bool refcount_is_one);
    
    
        //
        // find_large
        //
        // Find a large block in this zone.
        //
        inline Large *find_large(void *block) { return Large::large(block); }


        //
        // deallocate_small_medium
        //
        // Release memory allocated for a small block
        //
        void deallocate_small_medium(void *block);
        

      public:
      
        //
        // raw memory allocation
        //

#if UseArena
        
        // set our one region up
        void *arena_allocate_region(usword_t newsize);
#endif

        // on 32-bit w/o arena, goes directly to vm system
        // w/arena, allocate from the top of the arena
        void *arena_allocate_large(usword_t size);
        
        //
        // raw memory deallocation
        //
        void arena_deallocate(void *, size_t size);
        
        //
        // admin_offset
        //
        // Return the number of bytes to the beginning of the first admin data item.
        //
        static inline const usword_t admin_offset() { return align(sizeof(Zone), page_size); }
        

        //
        // bytes_needed
        // 
        // Calculate the number of bytes needed for zone data
        //
        static inline const usword_t bytes_needed() {
            usword_t in_subzone_size = Bitmap::bytes_needed(subzone_quantum_max);
            usword_t in_large_size = Bitmap::bytes_needed(allocate_quantum_large_max);
#if UseArena
            usword_t arena_size = Bitmap::bytes_needed(allocate_quantum_large_max);
#else
            usword_t arena_size = 0;
#endif
            return admin_offset() + in_subzone_size + in_large_size + arena_size;
        }


        //
        // allocator
        //
        inline void *operator new(const size_t size) {
#if DEBUG
            // allocate zone data
            void *allocation_address = allocate_guarded_memory(bytes_needed());
#else
            void *allocation_address = allocate_memory(bytes_needed());
#endif
        
            if (!allocation_address) error("Can not allocate zone");
            
            return allocation_address;

        }


        //
        // deallocator
        //
        inline void operator delete(void *zone) {
#if DEBUG
            // release zone data
            if (zone) deallocate_guarded_memory(zone, bytes_needed());
#else
            if (zone) deallocate_memory(zone, bytes_needed());
#endif
        }
       
      
        //
        // setup_shared
        //
        // Initialize information used by all zones.
        //
        static void setup_shared();
        
        //
        // allocate_thread_key
        //
        // attempt to allocate a static pthread key for use when creating a new zone
        // returns the new key, or 0 if no keys are available.
        //
        static pthread_key_t allocate_thread_key();

        //
        // Constructors
        //
        Zone(pthread_key_t thread_registration_key);
        
        
        //
        // Destructor
        //
        ~Zone();
        

        //
        // zone
        //
        // Returns the lowest index zone - for debugging purposes only (no locks.)
        //
        static inline Zone *zone() { return _first_zone; }


        //
        // Accessors
        //
        inline Admin *small_admin()                         { return &_small_admin; }
        inline Admin *medium_admin()                        { return &_medium_admin; }
        inline Thread         *threads()                    { return _registered_threads; }
        inline pthread_mutex_t *threads_mutex()             { return &_registered_threads_mutex; }
        inline Region         *region_list()                { return _region_list; }
        inline Large          *large_list()                 { return _large_list; }
        inline spin_lock_t    *large_lock()                 { return &_large_lock; }
        inline Statistics     &statistics()                 { return _stats; }
        inline Range          &coverage()                   { return _coverage; }
        inline TrackedPageAllocator &page_allocator()       { return _page_allocator; }
        inline PointerList    &garbage_list()               { return _garbage_list; }
        
        
        inline Thread *current_thread_direct() {
            if (_pthread_has_direct_tsd()) {
                #define CASE_FOR_DIRECT_KEY(key) case key: return (Thread *)_pthread_getspecific_direct(key)
                switch (_registered_threads_key) {
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY0);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY1);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY2);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY3);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY4);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY5);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY6);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY7);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY8);
                CASE_FOR_DIRECT_KEY(__PTK_FRAMEWORK_GC_KEY9);
                default: return NULL;
                }
            } else {
                return (Thread *)pthread_getspecific(_registered_threads_key);
            }
        }
        
        //
        // current_thread
        //
        // If the calling thread is registered with the collector, returns the registered Thread object.
        // If the calling thread is not registered, returns NULL.
        //
        inline Thread *current_thread() {
            Thread *thread = current_thread_direct();
            if (__builtin_expect(thread == INVALID_THREAD_KEY_VALUE, 0)) {
                // If we see this then it means some pthread destructor ran after the 
                // zone's destructor and tried to look up a Thread object (tried to perform a GC operation).
                // The collector's destructor needs to run last. We treat this as a fatal error so we will notice immediately.
                // Investigate as a pthreads bug in the ordering of static (Apple internal) pthread keys.
                auto_fatal("Zone::current_thread(): pthread looked up after unregister. Pthreads static key destructor ordering issue?\n");
            }
            return thread;
        }
        
        //
        // registered_thread
        //
        // Returns the Thread object for the calling thread.
        // If the calling thread is not registered, it is registered implicitly, and if warn_if_unregistered is true an error message is logged.
        //
        inline Thread &registered_thread() {
            Thread *thread = current_thread();
            if (!thread) {
                auto_error(this, "GC operation on unregistered thread. Thread registered implicitly. Break on auto_zone_thread_registration_error() to debug.\n", NULL);
                auto_zone_thread_registration_error();
                return register_thread();
            }
            return *thread;
        }
        
        //
        // destroy_registered_thread
        //
        // Pthread key destructor. The collector has a critical dependency on the ordering of pthread destructors.
        // This destructor must run after any other code which might possibly call into the collector.
        // We have arranged with pthreads to have our well known key destructor called after any dynamic keys and
        // any static (Apple internal) keys that might call into the collector (Foundation, CF). On the last iteration
        // (PTHREAD_DESTRUCTOR_ITERATIONS) we unregister the thread. 
        // Note that this implementation is non-portable due to our agreement with pthreads.
        //
        static void destroy_registered_thread(void *key_value);

        inline ScanStack      &scan_stack()                 { return _scan_stack; }
        inline void           set_state(enum State ns)      { _state = ns; }
        inline bool           is_state(enum State ns)       { return _state == ns; }
        
        inline spin_lock_t     *associations_lock()         { return &_associations_lock; }
        inline AssocationsHashMap  &assocations()               { return _associations; }

#if UseArena
        inline void *         arena()                       { return _arena; }
#else
        inline void *         arena()                       { return (void *)0; }
#endif
                
        inline bool           threshold_reached() const       { return _allocation_threshold <= 0; }  // threshold triggers GC as it crosses below zero
        inline void           reset_threshold()             { _allocation_threshold = control.collection_threshold; }
        inline void           adjust_threshold_counter(usword_t n)  { _allocation_threshold -= n; }
        
        //
        // subzone_index
        //
        // Returns a subzone index for an arbitrary pointer.  Note that this is relative to absolute memory.  subzone_index in
        // Region is relative memory. 
        //
        static inline const usword_t subzone_index(void *address) { return (((usword_t)address & mask(arena_size_log2)) >> subzone_quantum_log2); }
        
        
        //
        // subzone_count
        //
        // Returns a number of subzone quantum for a given size.
        //
        static inline const usword_t subzone_count(const size_t size) { return partition2(size, subzone_quantum_log2); }


        //
        // activate_subzone
        //
        // Marks the subzone as being active.
        //
        inline void activate_subzone(Subzone *subzone) { _in_subzone.set_bit_atomic(subzone_index(subzone)); }
        
        
        //
        // address_in_arena
        //
        // Given arbitrary address, is it in the arena of GC allocated memory
        //
        inline bool address_in_arena(void *address) const {
#if UseArena
            //return (((usword_t)address) >> arena_size_log2) == (((usword_t)_arena) >> arena_size_log2);
            return ((usword_t)address & ~mask(arena_size_log2)) == (usword_t)_arena;
#else
            return true;
#endif
        }
        
        
        //
        // in_subzone_memory
        //
        // Returns true if address is in auto managed memory.
        //
        inline const bool in_subzone_memory(void *address) const { return address_in_arena(address) && (bool)_in_subzone.bit(subzone_index(address)); }
        
        
        //
        // in_large_memory
        //
        // Returns true if address is in auto managed memory.  Since side data is smaller than a large quantum we'll not
        // concern ourselves with rounding.
        //
        inline const bool in_large_memory(void *address) const { return address_in_arena(address) && (bool)_in_large.bit(Large::quantum_index(address)); }
        
        
        //
        // in_zone_memory
        //
        // Returns true in address is in auto managed memory.
        //
        inline const bool in_zone_memory(void *address) const { return in_subzone_memory(address) || in_large_memory(address); }
        
        
        //
        // good_block_size
        //
        // Return a block size which maximizes memory usage (no slop.)
        //
        static inline const usword_t good_block_size(usword_t size) {
            if (size <= allocate_quantum_large)  return align2(size, allocate_quantum_medium_log2);
            return align2(size, allocate_quantum_small_log2);
        }
        
        
        //
        // is_block
        //
        // Determines if the specfied address is a block in this zone.
        //
        inline bool is_block(void *address) {
            return _coverage.in_range(address) && block_is_start(address);
        }
        
        
        //
        // block_allocate
        //
        // Allocate a block of memory from the zone.  layout indicates whether the block is an
        // object or not and whether it is scanned or not.
        //
        void *block_allocate(Thread &thread, const size_t size, const unsigned layout, const bool clear, bool refcount_is_one);

        
        //
        // block_deallocate
        //
        // Release a block of memory from the zone, lazily while scanning.
        // 
        void block_deallocate(void *block);
         

        //
        // block_is_start
        //
        // Return if the arbitrary address is the start of a block.
        // Broken down because of high frequency of use.
        //
        inline bool block_is_start(void *address) {
            if (in_subzone_memory(address)) {
                return Subzone::subzone(address)->block_is_start(address);
            } else if (in_large_memory(address)) {
                return Large::is_start(address);
            }
            return false;
        }
        

        //
        // block_start_large
        // 
        // Return the start of a large block.
        //
        void *block_start_large(void *address);
        
        
        //
        // block_start
        //
        // Return the base block address of an arbitrary address.
        // Broken down because of high frequency of use.
        //
        void *block_start(void *address);


        //
        // block_size
        //
        // Return the size of a specified block.
        //
        usword_t block_size(void *address);


        //
        // block_layout
        //
        // Return the layout of a block.
        //
        int block_layout(void *address);


        //
        // block_set_layout
        //
        // Set the layout of a block.
        //
        void block_set_layout(void *address, int layout);


        //
        // get_refcount_small_medium
        //
        // Return the refcount of a candidate small/medium block.
        //
        int get_refcount_small_medium(Subzone *subzone, void *address);
            
        
      private:
        //
        // inc_refcount_small_medium
        //
        // Increments the refcount of a small/medium block, returning the new value.
        // Requires subzone->admin()->lock() to be held, to protect side data.
        //
        int inc_refcount_small_medium(Subzone *subzone, void *block);
        
        
        //
        // dec_refcount_small_medium
        //
        // Decrements the refcount of a small/medium block, returning the new value.
        // Requires subzone->admin()->lock() to be held, to protect side data.
        //
        int dec_refcount_small_medium(Subzone *subzone, void *block);
        
        //
        // close_locks
        //
        // acquires all locks for critical sections whose behavior changes during scanning
        // enlivening_lock is and must already be held; all other critical sections must
        // order their locks with enlivening_lock acquired first
        //
        inline void close_locks() {
                // acquire all locks for sections that have predicated enlivening work
            // (These locks are in an arbitary order)

            spin_lock(_small_admin.lock());
            spin_lock(_medium_admin.lock());
            
            // Eventually we'll acquire these as well as we reintroduce ConditionBarrier
            //spin_lock(&_retains_lock);          // retain/release
            //spin_lock(&weak_refs_table_lock);   // weak references
            //spin_lock(&_associations_lock);     // associative references
            //spin_lock(&_roots_lock);            // global roots
        }
        
        inline void open_locks() {
            //spin_unlock(&_roots_lock);
            //spin_unlock(&_associations_lock);
            //spin_unlock(&weak_refs_table_lock);
            //spin_unlock(&_retains_lock);
            spin_unlock(_medium_admin.lock());
            spin_unlock(_small_admin.lock());
         }
        
      public:
      
        //
        // is_locked
        //
        // Called by debuggers, with all other threads suspended, to determine if any locks are held that might cause a deadlock from this thread.
        //
        bool is_locked();
      
      
        //
        // add_subzone
        //
        // when out of subzones, add another one, allocating region if necessary
        // return false if region can't be allocated
        //
        bool add_subzone(Admin *admin);

        //
        // block_refcount
        //
        // Returns the reference count of the specified block.
        //
        int block_refcount(void *block); 


        //
        // block_increment_refcount
        //
        // Increment the reference count of the specified block.
        //
        int block_increment_refcount(void *block); 


        //
        // block_decrement_refcount
        //
        // Decrement the reference count of the specified block.
        //
        int block_decrement_refcount(void *block);
        
        //
        // block_refcount_and_layout
        //
        // Accesses the reference count and layout of the specified block.
        //
        void block_refcount_and_layout(void *block, int *refcount, int *layout);
        
        //
        // is_new
        //
        // Returns true if the known-to-be-a-block was recently created.
        //
        inline bool is_new(void *block) {
            if (in_subzone_memory(block)) {
                return Subzone::subzone(block)->is_new(block);
            } else if (in_large_memory(block) && Large::is_start(block)) {
                return Large::is_new(block);
            }
            return false;
        }
        
        
        //
        // is_local
        //
        // Returns true if the known-to-be-a-block is a thread local node.
        //
        inline bool is_local(void *block) {
            if (in_subzone_memory(block)) {
                return Subzone::subzone(block)->is_thread_local(block);
            } 
            return false;
        }


        //
        // block_is_garbage
        //
        // Returns true if the specified block is flagged as garbage.  Only valid 
        // during finalization.
        //
        inline bool block_is_garbage(void *block) {
            if (in_subzone_memory(block)) {
                Subzone *subzone = Subzone::subzone(block);
                return subzone->block_is_start(block) && subzone->is_garbage(block);
            } else if (in_large_memory(block) && Large::is_start(block)) {
                return Large::large(block)->is_garbage();
            }

            return false;
        }
        
        //
        // block_is_marked
        //
        // if the block is already marked, say so.
        // Blocks are marked only by the collector thread in an unsynchronized way, so this may return false
        // if the collector's memmory write hasn't propogated to the caller's cpu.
        inline bool block_is_marked(void *block) {
            if (in_subzone_memory(block)) {
                Subzone *subzone = Subzone::subzone(block);
                return subzone->block_is_start(block) && subzone->is_marked(block);
            } else if (in_large_memory(block) && Large::is_start(block)) {
                return Large::large(block)->is_marked();
            }
            return false;
        }
        
        //
        // associations_should_be_marked
        //
        // Predicate to test whether a block's associations should be marked.
        //
        inline bool associations_should_be_marked(void *address) {
            if (in_subzone_memory(address)) {
                Subzone *subzone = Subzone::subzone(address);
                return subzone->block_is_start(address) && subzone->is_marked(address);
            } else if (in_large_memory(address) && Large::is_start(address)) {
                return Large::large(address)->is_marked();
            }
            // <rdar://problem/6463922> Treat non-block pointers as unconditionally live.
            return true;
        }


        //
        // set_associative_ref
        //
        // Creates an association between a given block, a unique pointer-sized key, and a pointer value.
        //
        void set_associative_ref(void *block, void *key, void *value);

        
        //
        // get_associative_ref
        //
        // Returns the associated pointer value for a given block and key.
        //
        void *get_associative_ref(void *block, void *key);

                
        //
        // erase_associations_internal
        //
        // Assuming association lock held, do the dissassociation dance
        //
        void erase_associations_internal(void *block);
        
        //
        // erase_assocations
        //
        // Removes all associations for a given block. Used to
        // clear associations for explicitly deallocated blocks.
        // When the collector frees blocks, it uses a different code
        // path, to minimize locking overhead. See free_garbage().
        //
        void erase_associations(void *block);

        //
        // erase_associations_in_range
        //
        // Called by remove_datasegment() below, when a data segment is unloaded
        // to automatically break associations referenced by global objects (@string constants).
        //
        void erase_associations_in_range(const Range &r);
        
        //
        // scan_assocations
        //
        // Called by MemoryScanner classes to enliven associative references.
        //
        void scan_associations(MemoryScanner &scanner);

        //
        // pend_associations
        //
        // Called during associative reference scanning to scan a newly discovered blocks
        // associations.
        //
        void pend_associations(void *block, MemoryScanner &scanner);
        
        //
        // add_root
        //
        // Adds the address as a known root.
        // Performs the assignment in a race-safe way.
        // Escapes thread-local value if necessary.
        //
        inline void add_root(void *root, void *value) {
            Thread &thread = registered_thread();
            thread.block_escaped(this, NULL, value);
            
            EnliveningHelper<UnconditionalBarrier> barrier(thread);
            SpinLock lock(&_roots_lock);
            if (_roots.find(root) == _roots.end()) {
                _roots.insert(root);
            }
            // whether new or old, make sure it gets scanned
            // if new, well, that's obvious, but if old the scanner may already have scanned
            // this root and we'll never see this value otherwise
            if (barrier && !block_is_marked(value)) barrier.enliven_block(value);
            *(void **)root = value;
        }
        
        
        //
        // add_root_no_barrier
        //
        // Adds the address as a known root.
        //
        inline void add_root_no_barrier(void *root) {
#if DEBUG
            // this currently fires if somebody uses the wrong version of objc_atomicCompareAndSwap*
            //if (in_subzone_memory(root)) __builtin_trap();
#endif

            SpinLock lock(&_roots_lock);
            if (_roots.find(root) == _roots.end()) {
                _roots.insert(root);
            }
        }
        
        
        // copy_roots
        //
        // Takes a snapshot of the registered roots during scanning.
        //
        inline void copy_roots(PointerList &list) {
            SpinLock lock(&_roots_lock);
            usword_t count = _roots.size();
            list.clear_count();
            list.grow(count);
            list.set_count(count);
            std::copy(_roots.begin(), _roots.end(), (void**)list.buffer());
        }
        
        // copy_roots
        //
        // Takes a snapshot of the registered roots during scanning.
        //
        inline void copy_roots(PtrVector &list) {
            SpinLock lock(&_roots_lock);
            usword_t count = _roots.size();
            list.resize(count);
            std::copy(_roots.begin(), _roots.end(), list.begin());
        }

        // remove_root
        //
        // Removes the address from the known roots.
        //
        inline void remove_root(void *root) {
            SpinLock lock(&_roots_lock);
            PtrHashSet::iterator iter = _roots.find(root);
            if (iter != _roots.end()) {
                _roots.erase(iter);
            }
        }
        
        
        //
        // is_root
        //
        // Returns whether or not the address has been registered.
        //
        inline bool is_root(void *address) {
            SpinLock lock(&_roots_lock);
            PtrHashSet::iterator iter = _roots.find(address);
            return (iter != _roots.end());
        }

        //
        // RangeLess
        //
        // Compares two ranges, returning true IFF r1 is left of r2 on the number line.
        // Returns false if the ranges overlap in any way.
        //
        struct RangeLess {
          bool operator()(const Range &r1, const Range &r2) const {
            return (r1.address() < r2.address()) && (r1.end() <= r2.address()); // overlapping ranges will always return false.
          }
        };
        
        //
        // add_datasegment
        //
        // Adds the given data segment address range to a list of known data segments, which is searched by is_global_address().
        //
        inline void add_datasegment(const Range &r) {
            SpinLock lock(&_datasegments_lock);
            RangeVector::iterator i = std::lower_bound(_datasegments.begin(), _datasegments.end(), r, RangeLess());
            _datasegments.insert(i, r);
        }
        
        //
        // RangeExcludes
        //
        // Returns false if the address lies outside the given range.
        //
        struct RangeExcludes {
            Range _range;
            RangeExcludes(const Range &r) : _range(r) {}
            bool operator()(void *address) { return !_range.in_range(address); }
        };

        //
        // RootRemover
        //
        // Used by remove_datasegment() below, removes an address from the
        // root table. Simply an artifact for use with std::for_each().
        //
        struct RootRemover {
            PtrHashSet &_roots;
            RootRemover(PtrHashSet &roots) : _roots(roots) {}
            void operator()(void *address) { 
                PtrHashSet::iterator iter = _roots.find(address);
                if (iter != _roots.end()) _roots.erase(iter);
            }
        };
    
        //
        // remove_datasegment
        //
        // Removes the given data segment address range from the list of known address ranges.
        //
        inline void remove_datasegment(const Range &r) {
            {
                SpinLock lock(&_datasegments_lock);
                // could use std::lower_bound(), or std::equal_range() to speed this up, since they use binary search to find the range.
                // _datasegments.erase(std::remove(_datasegments.begin(), _datasegments.end(), r, _datasegments.end()));
                RangeVector::iterator i = std::lower_bound(_datasegments.begin(), _datasegments.end(), r, RangeLess());
                if (i != _datasegments.end()) _datasegments.erase(i);
            }
            {
                // When a bundle gets unloaded, scour the roots table to make sure no stale roots are left behind.
                SpinLock lock(&_roots_lock);
                PtrVector rootsToRemove;
                std::remove_copy_if(_roots.begin(), _roots.end(), std::back_inserter(rootsToRemove), RangeExcludes(r));
                std::for_each(rootsToRemove.begin(), rootsToRemove.end(), RootRemover(_roots));
            }
            erase_associations_in_range(r);
            weak_unregister_data_segment(this, r.address(), r.size());
        }
        
        inline void add_datasegment(void *address, size_t size) { add_datasegment(Range(address, size)); }
        inline void remove_datasegment(void *address, size_t size) { remove_datasegment(Range(address, size)); }

        //
        // is_global_address
        //
        // Binary searches the registered data segment address ranges to determine whether the address could be referring to
        // a global variable.
        //
        inline bool is_global_address(void *address) {
            SpinLock lock(&_datasegments_lock);
            return std::binary_search(_datasegments.begin(), _datasegments.end(), Range(address, sizeof(void*)), RangeLess());
        }

#if DEBUG
        //
        // DATASEGMENT REGISTRATION UNIT TEST
        //
        struct RangePrinter {
            void operator() (const Range &r) {
                printf("{ address = %p, end = %p }\n", r.address(), r.end());
            }
        };
        
        inline void print_datasegments() {
            SpinLock lock(&_datasegments_lock);
            std::for_each(_datasegments.begin(), _datasegments.end(), RangePrinter());
        }

        void test_datasegments() {
            Range r1((void*)0x1000, 512), r2((void*)0xA000, 512);
            add_datasegment(r1);
            add_datasegment(r2);
            print_datasegments();
            Range r3(r1), r4(r2);
            r3.adjust(r1.size()), r4.adjust(-r2.size());
            add_datasegment(r3);
            add_datasegment(r4);
            print_datasegments();
            assert(is_global_address(r1.address()));
            assert(is_global_address(displace(r1.address(), 0x10)));
            assert(is_global_address(displace(r1.end(), -sizeof(void*))));
            assert(is_global_address(displace(r2.address(), 0xA0)));
            assert(is_global_address(displace(r3.address(), 0x30)));
            assert(is_global_address(displace(r4.address(), 0x40)));
            remove_datasegment(r2);
            print_datasegments();
            assert(!is_global_address(displace(r2.address(), 0xA0)));
            remove_datasegment(r1);
            assert(!is_global_address(displace(r1.address(), 0x10)));
            print_datasegments();
            remove_datasegment(r3);
            remove_datasegment(r4);
            print_datasegments();
        }
#endif

        //
        // erase_weak
        //
        // unregisters any weak references contained within known AUTO_OBJECT
        //
        inline void erase_weak(void *ptr) {
            if (control.weak_layout_for_address) {
                const unsigned char* weak_layout = control.weak_layout_for_address((auto_zone_t*)zone, ptr);
                if (weak_layout) weak_unregister_with_layout(this, (void**)ptr, weak_layout);
            }
        }

        //
        // add_zombie
        //
        // Adds address to the zombie set.
        //
        inline void add_zombie(void *address) {
            SpinLock lock(&_zombies_lock);
            if (_zombies.find(address) == _zombies.end()) {
                _zombies.insert(address);
            }
        }

        
        //
        // is_zombie
        //
        // Returns whether or not the address is in the zombie set.
        //
        inline bool is_zombie(void *address) {
            SpinLock lock(&_zombies_lock);
            PtrHashSet::iterator iter = _zombies.find(address);
            return (iter != _zombies.end());
        }
        
        //
        // clear_zombies
        //
        inline void clear_zombies() {
            SpinLock lock(&_zombies_lock);
            _zombies.clear();
        }
        
        
        //
        // set_write_barrier
        //
        // Set the write barrier byte corresponding to the specified address.
        // If scanning is going on then the value is marked pending.
        // If address is within an allocated block the value is set there and will return true.
        //
        bool set_write_barrier(Thread &thread, void *address, void *value);
        
        
        //
        // set_write_barrier_range
        //
        // Set the write barrier bytes corresponding to the specified address & length.
        // Returns if the address is within an allocated block (and barrier set)
        //
        bool set_write_barrier_range(void *address, const usword_t size);
        
        
        //
        // set_write_barrier
        //
        // Set the write barrier byte corresponding to the specified address.
        // Returns if the address is within an allocated block (and barrier set)
        //
        bool set_write_barrier(void *address);


        //
        // mark_write_barriers_untouched
        //
        // iterate through all the write barriers and mark the live cards as provisionally untouched.
        //
        void mark_write_barriers_untouched();


        //
        // clear_untouched_write_barriers
        //
        // iterate through all the write barriers and clear all the cards still marked as untouched.
        //
        void clear_untouched_write_barriers();


        //
        // clear_all_write_barriers
        //
        // iterate through all the write barriers and clear all the marks.
        //
        void clear_all_write_barriers();


        //
        // reset_all_marks
        //
        // Clears the mark flags on all blocks
        //
        void reset_all_marks();
       
        
        //
        // reset_all_marks_and_pending
        //
        // Clears the mark and ending flags on all blocks
        //
        void reset_all_marks_and_pending();
       
        
        //
        // statistics
        //
        // Returns the statistics for this zone.
        //
        void statistics(Statistics &stats);
        
        
        //
        //
        // scan_stack_push_block
        //
        // Push the block onto the scan stack.
        //
        void scan_stack_push_block(void *block) {
            _scan_stack.push(block);
        }


        //
        // scan_stack_push_range
        //
        // Push the range onto the scan stack.
        //
        void scan_stack_push_range(Range &range) {
            _scan_stack.push(range.end());
            _scan_stack.push(displace(range.address(), 1));
        }
        
        //
        // scan_stack_push_write_barrier
        //
        // Pushes a pointer to a WriteBarrier* that was in effect when a range was pushed.
        //
        void scan_stack_push_write_barrier(WriteBarrier *wb) {
            _scan_stack.push(displace(wb, 3));
        }
        
        
        //
        // scan_stack_is_empty
        //
        // Returns true if the stack is empty.
        //
        bool scan_stack_is_empty() { return _scan_stack.is_empty() || _scan_stack.is_overflow(); }
        
        
        //
        // scan_stack_is_range
        //
        // Returns true if the top of the scan stack is a range.
        //
        bool scan_stack_is_range() {
            void *pointer = _scan_stack.top();
            return (uintptr_t(pointer) & 0x3) == 0x1;
        }
        
        bool scan_stack_is_write_barrier() {
            void *pointer = _scan_stack.top();
            return (uintptr_t(pointer) & 0x3) == 0x3;
        }
        
        //
        // scan_stack_pop_block
        //
        // Return the next block to scan.
        //
        void *scan_stack_pop_block() {
            return _scan_stack.pop();
        }


        //
        // scan_stack_pop_range
        //
        // Return the next block to scan.
        //
        Range scan_stack_pop_range() {
            void *block1 = _scan_stack.pop();
            void *block2 = _scan_stack.pop();
            return Range(displace(block1, -1), block2);
        }
        
        WriteBarrier *scan_stack_pop_write_barrier() {
            void *pointer = _scan_stack.pop();
            return (WriteBarrier*) displace(pointer, -3);
        }

        
        //
        // scan_stack_max
        //
        // Return the maximum words used in this scan
        //
        unsigned long int scan_stack_max() { return _scan_stack.max(); }


        inline void set_repair_write_barrier(bool repair) { _repair_write_barrier = repair; }
        inline bool repair_write_barrier() const { return _repair_write_barrier; }
        
        
        //
        // set_needs_enlivening
        //
        // Inform all known threads that scanning is about to commence, thus blocks will need to be
        // enlivened to make sure they aren't missed during concurrent scanning.
        //
        void set_needs_enlivening();
        
        //
        // enlivening_barrier
        //
        // Called by Collector::scan_barrier() to enliven all blocks that
        // would otherwise be missed by concurrent scanning.
        //
        void enlivening_barrier(MemoryScanner &scanner);
        
        //
        // clear_needs_enlivening
        //
        // Unblocks threads that may be spinning waiting for enlivening to finish.
        //
        void clear_needs_enlivening();
        
        
        //
        // collect_begin
        //
        // Indicate the beginning of the collection period.  New blocks allocated during the time will
        // automatically marked and not treated as garbage.
        //
        inline void  collect_begin() {
        }
        
        
        //
        // collect_end
        //
        // Indicate the end of the collection period.  New blocks will no longer be marked automatically and will
        // be collected normally.
        //
        inline void  collect_end() {
            _garbage_list.uncommit();
            _small_admin.purge_free_space();
            _medium_admin.purge_free_space();
        }
        
        //
        // block_collector
        //
        // Called to lock the global mark bits and thread lists.
        // Returns true if successful.
        //
        bool block_collector();
        
        //
        // unblock_collector
        //
        // Called to unlock the global mark bits and thread lists.
        //
        void unblock_collector();
        
        //
        // collect
        //
        // Performs the collection process.
        //
        void collect(bool is_partial, void *current_stack_bottom, auto_date_t *scan_end);
        
        
        //
        // scavenge_blocks
        //
        // Constructs a list of all blocks that are to be garbaged
        //
        void scavenge_blocks();
        
        //
        // invalidate_garbage
        //
        // Given an array of garbage, do callouts for finalization
        //
        void invalidate_garbage(const size_t garbage_count, const vm_address_t *garbage);

        //
        // handle_overretained_garbage
        //
        // called when we detect a garbage block has been over retained during finalization
        // logs a (fatal, based on the setting) resurrection error
        // 
        void handle_overretained_garbage(void *block, int rc);
        
        //
        // free_garbage
        //
        // Given an array of garbage, free it en masse
        //
        size_t free_garbage(boolean_t generational, const size_t garbage_count, vm_address_t *garbage, size_t &blocks_freed, size_t &bytes_freed);
        
        //
        // release_pages
        //
        // Release any pages that are not in use.
        //
        void release_pages() {
        }
        
        //
        // recycle_threads
        //
        // Searches for unbound threads, queueing them for deletion.
        //
        void recycle_threads();
        
        //
        // register_thread
        //
        // Add the current thread as a thread to be scanned during gc.
        //
        Thread &register_thread();


        //
        // unregister_thread
        //
        // deprecated
        //
        void unregister_thread();

    private:
        Thread *firstScannableThread();
        Thread *nextScannableThread(Thread *thread);

    public:
        //
        // scan_registered_threads
        //
        // Safely enumerates the registered threads, ensuring that their stacks
        // remain valid during the call to the scanner block.
        //
        typedef void (^thread_scanner_t) (Thread *thread);
        void scan_registered_threads(thread_scanner_t scanner);
        void scan_registered_threads(void (*scanner) (Thread *, void *), void *arg) { scan_registered_threads(^(Thread *thread) { scanner(thread, arg); }); }
        

        //
        // suspend_all_registered_threads
        //
        // Suspend all registered threads. Provided for heap snapshots.
        // Acquires _registered_threads_lock so that no new threads can enter the system.
        //
        void suspend_all_registered_threads();


        //
        // resume_all_registered_threads
        //
        // Resumes all suspended registered threads.  Only used by the monitor for heap snapshots.
        // Relinquishes the _registered_threads_lock.
        //
        void resume_all_registered_threads();
        
        //
        // weak references.
        //
        unsigned has_weak_references() { return (num_weak_refs != 0); }

        //
        // layout_map_for_block.
        //
        // Used for precise (non-conservative) block scanning.
        //
        const unsigned char *layout_map_for_block(void *block) {
            // FIXME:  for speed, change this to a hard coded offset from the block's word0 field.
            return control.layout_for_address ? control.layout_for_address((auto_zone_t *)this, block) : NULL;
        }
        
        //
        // weak_layout_map_for_block.
        //
        // Used for conservative block with weak references scanning.
        //
        const unsigned char *weak_layout_map_for_block(void *block) {
            // FIXME:  for speed, change this to a hard coded offset from the block's word0 field.
            return control.weak_layout_for_address ? control.weak_layout_for_address((auto_zone_t *)this, block) : NULL;
        }

        //
        // print_all_blocks
        //
        // Prints all allocated blocks.
        //
        void print_all_blocks();
        
        
        //
        // print block
        //
        // Print the details of a block
        //
        void print_block(void *block);
        void print_block(void *block, const char *tag);
        
        //
        // malloc_statistics
        //
        // computes the necessary malloc statistics
        //
        void malloc_statistics(malloc_statistics_t *stats);

        //
        // dump
        //
        // call blocks with everything needed to recreate heap
        // blocks are called in the order given
        //
        void dump(
            auto_zone_stack_dump stack_dump,
            auto_zone_register_dump register_dump,
            auto_zone_node_dump thread_local_node_dump,
            auto_zone_root_dump root_dump,
            auto_zone_node_dump global_node_dump,
            auto_zone_weak_dump weak_dump_entry
        );

        
   };

};


#endif // __AUTO_ZONE_CORE__
