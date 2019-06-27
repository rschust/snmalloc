#pragma once

#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  struct SlabLink
  {
    SlabLink* prev;
    SlabLink* next;

    Slab* get_slab()
    {
      return pointer_cast<Slab>(address_cast(this) & SLAB_MASK);
    }
  };

  using SlabList = DLList<SlabLink>;

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab
  {
  private:
    // How many entries are used in this slab.
    uint16_t used = 0;

  public:
    // Bump free list of unused entries in this sizeclass.
    // If the bottom bit is 1, then this represents a bump_ptr
    // of where we have allocated up to in this slab. Otherwise,
    // it represents the location of the first block in the free
    // list.  The free list is chained through deallocated blocks.
    // It is terminated with a bump ptr.
    //
    // Note that, the first entry in a slab is never bump allocated
    // but is used for the link. This means that 1 represents the fully
    // bump allocated slab.
    Mod<SLAB_SIZE, uint16_t> head;
    // When a slab has free space it will be on the has space list for
    // that size class.  We use an empty block in this slab to be the
    // doubly linked node into that size class's free list.
    Mod<SLAB_SIZE, uint16_t> link;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;

    void add_use()
    {
      used++;
    }

    void sub_use()
    {
      used--;
    }

    void set_unused()
    {
      used = 0;
    }

    bool is_unused()
    {
      return used == 0;
    }

    bool is_full()
    {
      return link == 1;
    }

    void set_full()
    {
      assert(head == 1);
      assert(link != 1);
      link = 1;
    }

    SlabLink* get_link(Slab* slab)
    {
      return reinterpret_cast<SlabLink*>(pointer_offset(slab, link));
    }

    /// Value used to check for corruptions in a block
    static constexpr size_t POISON =
      static_cast<size_t>(bits::is64() ? 0xDEADBEEFDEAD0000 : 0xDEAD0000);

    /// Store next pointer in a block. In Debug using magic value to detect some
    /// simple corruptions.
    static void store_next(void* p, uint16_t head)
    {
#ifndef CHECK_CLIENT
      *static_cast<size_t*>(p) = head;
#else
      *static_cast<size_t*>(p) =
        head ^ POISON ^ (static_cast<size_t>(head) << (bits::BITS - 16));
#endif
    }

    /// Accessor function for the next pointer in a block.
    /// In Debug checks for simple corruptions.
    static uint16_t follow_next(void* node)
    {
      size_t next = *static_cast<size_t*>(node);
#ifdef CHECK_CLIENT
      if (((next ^ POISON) ^ (next << (bits::BITS - 16))) > 0xFFFF)
        error("Detected memory corruption.  Use-after-free.");
#endif
      return static_cast<uint16_t>(next);
    }
    bool valid_head(bool is_short)
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t slab_start = get_initial_link(sizeclass, is_short);
      size_t all_high_bits = ~static_cast<size_t>(1);

      size_t head_start =
        remove_cache_friendly_offset(head & all_high_bits, sizeclass);

      return ((head_start - slab_start) % size) == 0;
    }

    /**
     * Check bump-free-list-segment for cycles
     *
     * Using
     * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
     * We don't expect a cycle, so worst case is only followed by a crash, so
     * slow doesn't mater.
     **/
    void debug_slab_acyclic_free_list(Slab* slab)
    {
#ifndef NDEBUG
      uint16_t curr = head;
      uint16_t curr_slow = head;
      bool both = false;
      while ((curr & 1) != 1)
      {
        curr = follow_next(pointer_offset(slab, curr));
        if (both)
        {
          curr_slow = follow_next(pointer_offset(slab, curr_slow));
        }

        if (curr == curr_slow)
        {
          error("Free list contains a cycle, typically indicates double free.");
        }

        both = !both;
      }
#else
      UNUSED(slab);
#endif
    }

    void debug_slab_invariant(bool is_short, Slab* slab)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_initial_link(sizeclass, is_short);

      size_t accounted_for = used * size + offset;

      if (is_full())
      {
        // All the blocks must be used.
        assert(SLAB_SIZE == accounted_for);
        // There is no free list to validate
        // 'link' value is not important if full.
        return;
      }

      // Block is not full
      assert(SLAB_SIZE > accounted_for);

      debug_slab_acyclic_free_list(slab);

      // Walk bump-free-list-segment accounting for unused space
      uint16_t curr = head;
      while ((curr & 1) != 1)
      {
        // Check we are looking at a correctly aligned block
        uint16_t start = remove_cache_friendly_offset(curr, sizeclass);
        assert((start - offset) % size == 0);

        // Account for free elements in free list
        accounted_for += size;
        assert(SLAB_SIZE >= accounted_for);
        // We should never reach the link node in the free list.
        assert(curr != link);

        // Iterate bump/free list segment
        curr = follow_next(pointer_offset(slab, curr));
      }

      if (curr != 1)
      {
        // Check we terminated traversal on a correctly aligned block
        uint16_t start = remove_cache_friendly_offset(curr & ~1, sizeclass);
        assert((start - offset) % size == 0);

        // Account for to be bump allocated space
        accounted_for += SLAB_SIZE - (curr - 1);

        // The link should be the first allocation as we
        // haven't completely filled this block at any point.
        assert(link == get_initial_link(sizeclass, is_short));
      }

      assert(!is_full());
      // Add the link node.
      accounted_for += size;

      // All space accounted for
      assert(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
      UNUSED(is_short);
#endif
    }
  };
} // namespace snmalloc
