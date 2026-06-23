#pragma once

#include "../DXSampleHelper.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <vector>

// A slot-only handle returned by StagedDescriptorAllocator::Allocate().
// The Index is stable across Grow() calls; CPU/GPU descriptor handles
// for a given slot are obtained via StagedDescriptorAllocator::CpuHandle() /
// GpuHandle().
struct StagedDescriptorHandle
{
    UINT Index = UINT_MAX;

    bool IsValid() const
    {
        return Index != UINT_MAX;
    }
};

// A descriptor range returned by StagedDescriptorAllocator::AllocContiguous().
// Includes both the start slot and the count for safe freeing.
struct StagedDescriptorRange
{
    UINT Start = UINT_MAX;
    UINT Count = 0;

    bool IsValid() const
    {
        return Start != UINT_MAX;
    }
};

// A growable, staged descriptor allocator.
//
// Design:
//   CPU heap (D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
//     - Authoritative storage. All writes happen here.
//     - Free list tracks available slots.
//     - Grows on demand when the free list is exhausted.
//
//   GPU heap (D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
//     - Staging copy, refreshed each frame via CopyDescriptors.
//     - Resized to match CPU heap when growing.
//
//   Allocate() returns a slot-only StagedDescriptorHandle.
//   CPU/GPU descriptor handles are obtained via CpuHandle()/GpuHandle().
//
//   Stage(completedFenceValue) copies all currently allocated descriptors
//   from CPU → GPU and releases old GPU heaps whose fence has been reached.
//   Call once per frame before issuing draw/dispatch commands.
//
//   Allocate/AllocContiguous take a retireFenceValue parameter so the
//   old GPU heap (if Grow is triggered) is retained until the fence completes.
//
//   Free() returns the logical index to the free list.
//   In debug builds, double-free and out-of-range are detected via slot state tracking.
class StagedDescriptorAllocator
{
public:
    ~StagedDescriptorAllocator()
    {
        Destroy();
    }

    // Initialise with a starting capacity.
    // Must be called before any Allocate.
    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT initialCapacity)
    {
        assert(m_device == nullptr);
        m_device = device;
        m_heapType = type;
        m_increment = device->GetDescriptorHandleIncrementSize(type);
        m_capacity = 0;
        m_maxUsedIndex = 0;

        // Initialise with a dummy fence value; the first Grow will not have a
        // previous GPU heap to defer, so the value doesn't matter.
        Grow(initialCapacity, UINT64_MAX);
    }

    void Destroy()
    {
        m_device = nullptr;
        m_cpuHeap.Reset();
        m_gpuHeap.Reset();
        m_pendingGpuHeaps.clear();
        m_capacity = 0;
        m_increment = 0;
        m_maxUsedIndex = 0;
        m_freeIndices.clear();
        m_slotState.clear();
    }

    // Allocate one descriptor slot.
    // retireFenceValue is used if a Grow() is triggered, so the old GPU heap
    // is retained until the fence completes.
    StagedDescriptorHandle Allocate(UINT64 retireFenceValue)
    {
        assert(m_device != nullptr);

        if (m_freeIndices.empty())
        {
            UINT growSize = (std::max)(m_capacity, 64u);
            Grow(growSize, retireFenceValue);
        }

        UINT idx = m_freeIndices.back();
        m_freeIndices.pop_back();

        assert(idx < m_slotState.size());
        assert(m_slotState[idx] == SlotState::Free);
        m_slotState[idx] = SlotState::Allocated;

        StagedDescriptorHandle handle;
        handle.Index = idx;

        if (idx >= m_maxUsedIndex)
        {
            m_maxUsedIndex = idx + 1;
        }

        return handle;
    }

    // Stage: copy all live (used) descriptors from CPU heap to GPU heap,
    // and release old GPU heaps whose fence has completed.
    // Call once per frame before any draw/dispatch that references staged descriptors.
    void Stage(UINT64 completedFenceValue)
    {
        CollectCompletedGpuHeaps(completedFenceValue);

        if (m_maxUsedIndex == 0)
        {
            return;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE srcStart = m_cpuStart;

        D3D12_CPU_DESCRIPTOR_HANDLE dstStart = m_gpuCpuStart;

        m_device->CopyDescriptorsSimple(static_cast<UINT>(m_maxUsedIndex),
                                        dstStart,
                                        srcStart,
                                        m_heapType);
    }

    // Allocate a contiguous block of descriptor slots (for descriptor tables).
    // retireFenceValue is used if a Grow() is triggered, so the old GPU heap
    // is retained until the fence completes.
    StagedDescriptorRange AllocContiguous(UINT count, UINT64 retireFenceValue)
    {
        assert(m_device != nullptr);
        assert(count > 0);

        UINT start = FindContiguousRun(count);
        if (start == UINT_MAX)
        {
            UINT growSize = (std::max)(count, (std::max)(m_capacity, 64u));
            Grow(growSize, retireFenceValue);
            start = FindContiguousRun(count);
            assert(start != UINT_MAX);
        }

        // Remove the allocated slots from the free list and mark them allocated.
        for (UINT i = 0; i < count; ++i)
        {
            UINT slot = start + i;
            assert(slot < m_slotState.size());
            assert(m_slotState[slot] == SlotState::Free);

            auto it = std::find(m_freeIndices.begin(), m_freeIndices.end(), slot);
            assert(it != m_freeIndices.end());
            *it = m_freeIndices.back();
            m_freeIndices.pop_back();

            m_slotState[slot] = SlotState::Allocated;
        }

        if (start + count > m_maxUsedIndex)
        {
            m_maxUsedIndex = start + count;
        }

        StagedDescriptorRange range;
        range.Start = start;
        range.Count = count;
        return range;
    }

    // Compute the CPU descriptor handle for a logical slot.
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT slot) const
    {
        assert(slot < m_capacity);
        D3D12_CPU_DESCRIPTOR_HANDLE h = {};
        h.ptr = m_cpuStart.ptr + (slot * m_increment);
        return h;
    }

    // Compute the GPU descriptor handle for a logical slot.
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT slot) const
    {
        assert(slot < m_capacity);
        D3D12_GPU_DESCRIPTOR_HANDLE h = {};
        h.ptr = m_gpuStart.ptr + (slot * m_increment);
        return h;
    }

    void Free(StagedDescriptorHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }

        UINT idx = handle.Index;
        assert(idx < m_slotState.size());
        assert(m_slotState[idx] == SlotState::Allocated);

        m_slotState[idx] = SlotState::Free;
        m_freeIndices.push_back(idx);

        // Shrink maxUsedIndex when the highest slot is freed (optimistic).
        if (idx + 1 == m_maxUsedIndex)
        {
            while (m_maxUsedIndex > 0)
            {
                --m_maxUsedIndex;
                auto it = std::find(m_freeIndices.begin(), m_freeIndices.end(), m_maxUsedIndex);
                if (it == m_freeIndices.end())
                {
                    ++m_maxUsedIndex;
                    break;
                }
            }
        }
    }

    // Free a contiguous block of descriptor slots allocated via AllocContiguous.
    // `first` is the handle returned by AllocContiguous; `count` must match the
    // original allocation count.
    void FreeContiguous(StagedDescriptorHandle first, UINT count)
    {
        if (!first.IsValid() || count == 0)
        {
            return;
        }

        for (UINT i = 0; i < count; ++i)
        {
            UINT idx = first.Index + i;
            assert(idx < m_slotState.size());
            assert(m_slotState[idx] == SlotState::Allocated);

            m_slotState[idx] = SlotState::Free;
            m_freeIndices.push_back(idx);
        }

        // Shrink maxUsedIndex if the block ends at the top.
        UINT end = first.Index + count;
        if (end == m_maxUsedIndex)
        {
            while (m_maxUsedIndex > 0)
            {
                --m_maxUsedIndex;
                auto it = std::find(m_freeIndices.begin(), m_freeIndices.end(), m_maxUsedIndex);
                if (it == m_freeIndices.end())
                {
                    ++m_maxUsedIndex;
                    break;
                }
            }
        }
    }

    // Free a contiguous block of descriptor slots via a StagedDescriptorRange.
    void FreeContiguous(StagedDescriptorRange range)
    {
        if (!range.IsValid() || range.Count == 0)
        {
            return;
        }

        for (UINT i = 0; i < range.Count; ++i)
        {
            UINT idx = range.Start + i;
            assert(idx < m_slotState.size());
            assert(m_slotState[idx] == SlotState::Allocated);

            m_slotState[idx] = SlotState::Free;
            m_freeIndices.push_back(idx);
        }

        UINT end = range.Start + range.Count;
        if (end == m_maxUsedIndex)
        {
            while (m_maxUsedIndex > 0)
            {
                --m_maxUsedIndex;
                auto it = std::find(m_freeIndices.begin(), m_freeIndices.end(), m_maxUsedIndex);
                if (it == m_freeIndices.end())
                {
                    ++m_maxUsedIndex;
                    break;
                }
            }
        }
    }

    UINT Capacity() const { return m_capacity; }
    UINT Used() const { return m_capacity - static_cast<UINT>(m_freeIndices.size()); }
    UINT DescriptorIncrement() const { return m_increment; }
    ID3D12DescriptorHeap* GetGpuHeap() const { return m_gpuHeap.Get(); }

private:
    void Grow(UINT additionalSlots, UINT64 retireFenceValue)
    {
        UINT newCapacity = m_capacity + additionalSlots;

        // Create or grow CPU heap
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = m_heapType;
        desc.NumDescriptors = newCapacity;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 0;

        ComPtr<ID3D12DescriptorHeap> newCpuHeap;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&newCpuHeap)));
        newCpuHeap->SetName(L"StagedDescriptorAllocator (CPU)");

        // Copy existing descriptors from old CPU heap to new CPU heap
        if (m_cpuHeap != nullptr && m_maxUsedIndex > 0)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE srcStart = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE dstStart = newCpuHeap->GetCPUDescriptorHandleForHeapStart();
            m_device->CopyDescriptorsSimple(m_maxUsedIndex, dstStart, srcStart, m_heapType);
        }

        // Create or grow GPU heap
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> newGpuHeap;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&newGpuHeap)));
        newGpuHeap->SetName(L"StagedDescriptorAllocator (GPU)");

        if (m_gpuHeap != nullptr && m_maxUsedIndex > 0)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE srcStart = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE dstStart = newGpuHeap->GetCPUDescriptorHandleForHeapStart();
            m_device->CopyDescriptorsSimple(m_maxUsedIndex, dstStart, srcStart, m_heapType);
        }

        // Add new slots to the free list
        for (UINT i = m_capacity; i < newCapacity; ++i)
        {
            m_freeIndices.push_back(i);
        }

        // Extend slot state tracking for new slots.
        m_slotState.resize(newCapacity, SlotState::Free);

        // Defer-release old GPU heap so in-flight draws finish before destruction.
        if (m_gpuHeap != nullptr)
        {
            m_pendingGpuHeaps.push_back({m_gpuHeap, retireFenceValue});
        }

        m_cpuHeap.Swap(newCpuHeap);
        m_gpuHeap.Swap(newGpuHeap);
        m_cpuStart = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
        m_gpuStart = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
        m_gpuCpuStart = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
        m_capacity = newCapacity;
    }

    // Find the first contiguous run of `count` free slots.
    // Returns UINT_MAX if no such run exists.
    UINT FindContiguousRun(UINT count) const
    {
        if (m_freeIndices.size() < count)
        {
            return UINT_MAX;
        }

        std::vector<UINT> sorted = m_freeIndices;
        std::sort(sorted.begin(), sorted.end());

        // Single slot is always contiguous.
        if (count == 1)
        {
            return sorted[0];
        }

        UINT consecutive = 1;

        for (size_t i = 1; i < sorted.size(); ++i)
        {
            if (sorted[i] == sorted[i - 1] + 1)
            {
                ++consecutive;
                if (consecutive >= count)
                {
                    return sorted[i] - count + 1;
                }
            }
            else
            {
                consecutive = 1;
            }
        }

        return UINT_MAX;
    }

    // Release old GPU heaps whose fence has been reached.
    void CollectCompletedGpuHeaps(UINT64 completedFenceValue)
    {
        while (!m_pendingGpuHeaps.empty() &&
               m_pendingGpuHeaps.front().retireFenceValue <= completedFenceValue)
        {
            m_pendingGpuHeaps.pop_front();
        }
    }

    ID3D12Device* m_device = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE m_heapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;

    ComPtr<ID3D12DescriptorHeap> m_cpuHeap;
    ComPtr<ID3D12DescriptorHeap> m_gpuHeap;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_gpuCpuStart{};

    UINT m_increment = 0;
    UINT m_capacity = 0;

    // Highest index ever allocated + 1 (defines the copy range for Stage).
    UINT m_maxUsedIndex = 0;

    enum class SlotState
    {
        Free,
        Allocated,
    };
    std::vector<SlotState> m_slotState;
    std::vector<UINT> m_freeIndices;

    struct PendingGpuHeap
    {
        ComPtr<ID3D12DescriptorHeap> heap;
        UINT64 retireFenceValue;
    };
    std::deque<PendingGpuHeap> m_pendingGpuHeaps;
};
