// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#ifndef TCMALLOC_PAGE_HEAP_H_
#define TCMALLOC_PAGE_HEAP_H_

#include <config.h>
#include <stddef.h>                     // for size_t
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uint64_t, int64_t, uint16_t
#endif
#include <gperftools/malloc_extension.h>
#include "base/basictypes.h"
#include "common.h"
#include "packed-cache-inl.h"
#include "pagemap.h"
#include "span.h"

// We need to dllexport PageHeap just for the unittest.  MSVC complains
// that we don't dllexport the PageHeap members, but we don't need to
// test those, so I just suppress this warning.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4251)
#endif

// This #ifdef should almost never be set.  Set NO_TCMALLOC_SAMPLES if
// you're porting to a system where you really can't get a stacktrace.
// Because we control the definition of GetStackTrace, all clients of
// GetStackTrace should #include us rather than stacktrace.h.
#ifdef NO_TCMALLOC_SAMPLES
// We use #define so code compiles even if you #include stacktrace.h somehow.
# define GetStackTrace(stack, depth, skip)  (0)
#else
# include <gperftools/stacktrace.h>
#endif

namespace base {
	struct MallocRange;
}

namespace tcmalloc {

	// -------------------------------------------------------------------------
	// Map from page-id to per-page data
	// -------------------------------------------------------------------------

	// We use PageMap2<> for 32-bit and PageMap3<> for 64-bit machines.
	// We also use a simple one-level cache for hot PageID-to-sizeclass mappings,
	// because sometimes the sizeclass is all the information we need.

	// Selector class -- general selector uses 3-level map
	template <int BITS> class MapSelector {
		public:
			typedef TCMalloc_PageMap3<BITS-kPageShift> Type;
	};

#ifndef TCMALLOC_SMALL_BUT_SLOW
	// x86-64 and arm64 are using 48 bits of address space. So we can use
	// just two level map, but since initial ram consumption of this mode
	// is a bit on the higher side, we opt-out of it in
	// TCMALLOC_SMALL_BUT_SLOW mode.
	template <> class MapSelector<48> {
		public:
			typedef TCMalloc_PageMap2<48-kPageShift> Type;
	};

#endif // TCMALLOC_SMALL_BUT_SLOW

	// A two-level map for 32-bit machines
	template <> class MapSelector<32> {
		public:
			typedef TCMalloc_PageMap2<32-kPageShift> Type;
	};
	// -------------------------------------------------------------------------
	// Page-level allocator
	//  * Eager coalescing
	//
	// Heap for page-level allocation.  We allow allocating and freeing a
	// contiguous runs of pages (called a "span").
	// -------------------------------------------------------------------------

	class PERFTOOLS_DLL_DECL PageHeap {
		public:
			PageHeap();

			// Allocate a run of "n" pages.  Returns zero if out of memory.
			// Caller should not pass "n == 0" -- instead, n should have
			// been rounded up already.
			Span* New(Length n);

			struct SmallSpanStats {
				int64 normal_length;
				int64 returned_length;
			};
			void GetSmallSpanStats(SmallSpanStats* result);
			void AppendSpantoPageHeap(Span* span);
			bool Check();
			// Like Check() but does some more comprehensive checking.
			bool CheckExpensive();
			bool CheckList(Span* list, int freelist);  // ON_NORMAL_FREELIST or ON_RETURNED_FREELIST

			// taghavi
			// extended memory unit nested class
			class ExtendedMemory {
				public:
					ExtendedMemory();

					// Allocate a large span of length == n.  If successful, returns a
					// span of exactly the specified length.  Else, returns NULL.
					Span* AllocLarge(Length n);

					bool Check();

					// Try to release at least num_pages for reuse by the OS.  Returns
					// the actual number of pages released, which may be less than
					// num_pages if there weren't enough pages to release. The result
					// may also be larger than num_pages since page_heap might decide to
					// release one large range instead of fragmenting it into two
					// smaller released and unreleased ranges.
					Length ReleaseAtLeastNPages(Length num_pages);

					struct LargeSpanStats {
						int64 spans;           // Number of such spans
						int64 normal_pages;    // Combined page length of normal large spans
						int64 returned_pages;  // Combined page length of unmapped spans
					};
					void GetLargeSpanStats(LargeSpanStats* result);

					// Delete the span "[p, p+n-1]".
					// REQUIRES: span was returned by earlier call to New() and
					//           has not yet been deleted.
					void Delete(Span* span);

					// Split an allocated span into two spans: one of length "n" pages
					// followed by another span of length "span->length - n" pages.
					// Modifies "*span" to point to the first span of length "n" pages.
					// Returns a pointer to the second span.
					//
					// REQUIRES: "0 < n < span->length"
					// REQUIRES: span->location == IN_USE
					// REQUIRES: span->sizeclass == 0
					Span* Split(Span* span, Length n);
					bool CheckSet();

					bool GetAggressiveDecommit(void) {return aggressive_decommit_;}
					void SetAggressiveDecommit(bool aggressive_decommit) {
						aggressive_decommit_ = aggressive_decommit;
					}
				private:
					// Rather than using a linked list, we use sets here for efficient
					// best-fit search.
					SpanSet large_normal_;
					SpanSet large_returned_;

					bool GrowHeap(Length n);

					// return span.
					Span* CarveLarge(Span* span, Length n);

					// Removes span from its free list, and adjust stats.
					void RemoveFromFreeSet(Span* span);

					// Prepends span to appropriate free list, and adjusts stats.
					void PrependToFreeSet(Span* span);

					// Attempts to decommit 's' and move it to the returned freelist.
					//
					// Returns the length of the Span or zero if release failed.
					//
					// REQUIRES: 's' must be on the NORMAL freelist.
					Length ReleaseSpan(Span *s);

					// Coalesce span with neighboring spans if possible, prepend to
					// appropriate free list, and adjust stats.
					void MergeIntoFreeSet(Span* span);

					Span* CheckAndHandlePreMerge(Span *span, Span *other);
					bool aggressive_decommit_;
					void IncrementalScavenge(Length n);

					// Number of pages to deallocate before doing more scavenging
					int64_t scavenge_counter_;

					// Minimum number of pages to fetch from system at a time.  Must be
					// significantly bigger than kBlockSize to amortize system-call
					// overhead, and also to reduce external fragementation.  Also, we
					// should keep this value big because various incarnations of Linux
					// have small limits on the number of mmap() regions per
					// address-space.
					static const int kMinSystemAlloc = 2;

					// Allocates a big block of memory for the pagemap once we reach more than
					// 128MB
					static const size_t kPageMapBigAllocationThreshold = 128 << 20;

					// Never delay scavenging for more than the following number of
					// deallocated pages.  With 4K pages, this comes to 4GB of
					// deallocation.
					static const int kMaxReleaseDelay = 1 << 20;

					// If there is nothing to release, wait for so many pages before
					// scavenging again.  With 4K pages, this comes to 1GB of memory.
					static const int kDefaultReleaseDelay = 1 << 18;
			};

			class PageMap{
				public:
					PageMap();

					// Return the descriptor for the specified page.  Returns NULL if
					// this PageID was not allocated previously.
					inline ATTRIBUTE_ALWAYS_INLINE
						Span* GetDescriptor(PageID p) const {
							return reinterpret_cast<Span*>(pagemap_.get(p));
						}

					// Page heap statistics
					struct Stats {
						Stats() : system_bytes(0), free_bytes(0), unmapped_bytes(0), committed_bytes(0),
						scavenge_count(0), commit_count(0), total_commit_bytes(0),
						decommit_count(0), total_decommit_bytes(0),
						reserve_count(0), total_reserve_bytes(0) {}
						uint64_t system_bytes;    // Total bytes allocated from system
						uint64_t free_bytes;      // Total bytes on normal freelists
						uint64_t unmapped_bytes;  // Total bytes on returned freelists
						uint64_t committed_bytes;  // Bytes committed, always <= system_bytes_.

						uint64_t scavenge_count;   // Number of times scavagened flush pages

						uint64_t commit_count;          // Number of virtual memory commits
						uint64_t total_commit_bytes;    // Bytes committed in lifetime of process
						uint64_t decommit_count;        // Number of virtual memory decommits
						uint64_t total_decommit_bytes;  // Bytes decommitted in lifetime of process

						uint64_t reserve_count;         // Number of virtual memory reserves
						uint64_t total_reserve_bytes;   // Bytes reserved in lifetime of process
					};
					inline Stats stats() const { return stats_; }

					uint64_t GetSystemBytes(){ return stats_.system_bytes; } 
					uint64_t GetFreeBytes(){ return stats_.free_bytes; } 
					uint64_t GetUnmappedBytes(){ return stats_.unmapped_bytes; } 
					uint64_t GetCommitedBytes(){ return stats_.committed_bytes; }
					void AddFreeBytes(uint64_t val){ stats_.free_bytes += val; }
					void AddSystemBytes(uint64_t val){ stats_.system_bytes += val; }
					void AddUnmappedBytes(uint64_t val){ stats_.unmapped_bytes += val; }
					void AddCommitedBytes(uint64_t val){ stats_.committed_bytes += val; }
					void AddTotalCommitBytes(uint64_t val){ stats_.total_commit_bytes += val; }
					void AddTotalReserveBytes(uint64_t val){ stats_.total_reserve_bytes += val; }	
					void ReduceFreeBytes(uint64_t val){ stats_.free_bytes -= val; }	
					void ReduceUnmappedBytes(uint64_t val){ stats_.unmapped_bytes -= val; }	
					void AddScavengeCount(uint64_t val){ stats_.scavenge_count += val; }	
					void AddReserveCount(uint64_t val){ stats_.reserve_count += val; }	
					void AddCommitCount(uint64_t val){ stats_.commit_count += val; }	

					// Reads and writes to pagemap_cache_ do not require locking.
					bool TryGetSizeClass(PageID p, uint32* out) const {
						return pagemap_cache_.TryGet(p, out);
					}
					void SetCachedSizeClass(PageID p, uint32 cl) {
						ASSERT(cl != 0);
						pagemap_cache_.Put(p, cl);
					}
					void InvalidateCachedSizeClass(PageID p) { pagemap_cache_.Invalidate(p); }
					uint32 GetSizeClassOrZero(PageID p) const {
						uint32 cached_value;
						if (!TryGetSizeClass(p, &cached_value)) {
							cached_value = 0;
						}
						return cached_value;
					}
					// Mark an allocated span as being used for small objects of the
					// specified size-class.
					// REQUIRES: span was returned by an earlier call to New()
					//           and has not yet been deleted.
					void RegisterSizeClass(Span* span, uint32 sc);

					// If this page heap is managing a range with starting page # >= start,
					// store info about the range in *r and return true.  Else return false.
					bool GetNextRange(PageID start, base::MallocRange* r);
					typedef uintptr_t Number;

					void RecordSpan(Span* span) {
						pagemap_.set(span->start, span);
						if (span->length > 1) {
							pagemap_.set(span->start + span->length - 1, span);
						}
					}

					// Commit the span.
					void CommitSpan(Span* span);

					// Checks if we are allowed to take more memory from the system.
					// If limit is reached and allowRelease is true, tries to release
					// some unused spans.
					bool EnsureLimit(Length n, bool allowRelease = true);

					// Decommit the span.
					bool DecommitSpan(Span* span);

					void SetPageMap(Number k, void* v);
					void* NextPageMap(Number k);
					void PreallocateMoreMemoryPageMap();
					bool EnsurePageMap(Number start, size_t n);
				private:
					// Pick the appropriate map and cache types based on pointer size
					typedef MapSelector<kAddressBits>::Type PageMapType;
					typedef PackedCache<kAddressBits - kPageShift> PageMapCache;
					mutable PageMapCache pagemap_cache_;
					PageMapType pagemap_;

					// Statistics on system, free, and unmapped bytes
					Stats stats_;
			};

		private:
			// We segregate spans of a given size into two circular linked
			// lists: one for normal spans, and one for spans whose memory
			// has been returned to the system.
			struct SpanList {
				Span        normal;
				Span        returned;
			};

			// Array mapping from span length to a doubly linked list of free spans
			SpanList free_;

			// Prepends span to appropriate free list, and adjusts stats.
			void PrependToFreeList(Span* span);

			// Removes span from its free list, and adjust stats.
			void RemoveFromFreeList(Span* span);
	};

}  // namespace tcmalloc

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif  // TCMALLOC_PAGE_HEAP_H_
