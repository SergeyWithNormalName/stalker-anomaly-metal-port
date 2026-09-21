#pragma once

#if defined(_MSC_VER) && !defined(USE_TBB_CONCURRENCY)
#include <ppl.h>
#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>
#include <atomic>
#include <functional>
#include <algorithm>
#include <iterator>
#include <type_traits>

// Atomic types
using xr_atomic_u32 = std::atomic_uint32_t;
using xr_atomic_s32 = std::atomic_int;
using xr_atomic_bool = std::atomic_bool;

// Tasks Redefinition
using xr_task_group = concurrency::task_group;

template <typename T, typename U>
using xr_concurrent_unordered_map = concurrency::concurrent_unordered_map<T, U>;

template <typename T, typename allocator = xalloc<T>>
using xr_concurrent_vector = concurrency::concurrent_vector<T, allocator>;

template<typename BlockRangeType, typename Body>
inline void xr_parallel_for(BlockRangeType Begin, BlockRangeType End, Body Functor)
{
	concurrency::parallel_for(Begin, End, Functor);
}

template<typename Index, typename Body>
inline void xr_parallel_foreach(Index Begin, Index End, Body Functor)
{
	concurrency::parallel_for_each(Begin, End, Functor);
}
// Helper to deduce the value type for the default predicate
template <typename RandomIt>
using IterValueT = typename std::iterator_traits<RandomIt>::value_type;

// Helper to check if an iterator is Random Access
template <typename It>
using IsRandomAccess = std::is_base_of<
    std::random_access_iterator_tag,
    typename std::iterator_traits<It>::iterator_category
>;

// PPL behaviour - fallback to std::sort if chunk size < 2048 and cores < 2
template<typename RandomIt, typename P = std::less<IterValueT<RandomIt>>>
IC void xr_parallel_sort(RandomIt first, RandomIt last, P pred = {})
{
    concurrency::parallel_sort(first, last, pred);
}
#else
// Cross-platform Intel TBB implementation for macOS / POSIX / Clang.
// Apple builds use Homebrew oneTBB, rather than the bundled 2020 headers.
#if defined(__APPLE__)
#include <oneapi/tbb.h>
#include <oneapi/tbb/task_group.h>
#include <oneapi/tbb/concurrent_unordered_map.h>
#include <oneapi/tbb/concurrent_vector.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/parallel_sort.h>
#else
#include <tbb/tbb.h>
#include <tbb/task_group.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>
#endif
#include <atomic>
#include <functional>
#include <algorithm>
#include <iterator>
#include <type_traits>

// Atomic types
using xr_atomic_u32 = std::atomic_uint32_t;
using xr_atomic_s32 = std::atomic_int;
using xr_atomic_bool = std::atomic_bool;

// Tasks Redefinition
using xr_task_group = tbb::task_group;

template <typename T, typename U>
using xr_concurrent_unordered_map = tbb::concurrent_unordered_map<T, U>;

template <typename T, typename allocator = tbb::tbb_allocator<T>>
using xr_concurrent_vector = tbb::concurrent_vector<T, allocator>;

template<typename BlockRangeType, typename Body>
inline void xr_parallel_for(BlockRangeType Begin, BlockRangeType End, Body Functor)
{
	tbb::parallel_for(Begin, End, Functor);
}

template<typename Index, typename Body>
inline void xr_parallel_foreach(Index Begin, Index End, Body Functor)
{
	tbb::parallel_for_each(Begin, End, Functor);
}

// Helper to deduce the value type for the default predicate
template <typename RandomIt>
using IterValueT = typename std::iterator_traits<RandomIt>::value_type;

// Helper to check if an iterator is Random Access
template <typename It>
using IsRandomAccess = std::is_base_of<
    std::random_access_iterator_tag,
    typename std::iterator_traits<It>::iterator_category
>;

template<typename RandomIt, typename P = std::less<IterValueT<RandomIt>>>
IC void xr_parallel_sort(RandomIt first, RandomIt last, P pred = {})
{
    tbb::parallel_sort(first, last, pred);
}
#endif
