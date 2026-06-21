#include "stdafx.h"

#include "StagedDescriptorAllocator.h"

#include <cstdio>

// Quick smoke test for StagedDescriptorAllocator.
// Verifies allocation, staging, freeing, and growth.
// Returns true on success.
static bool RunStagedAllocatorTest(ID3D12Device* device)
{
    StagedDescriptorAllocator alloc;

    alloc.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);

    // Allocate 3 slots (retireFenceValue=UINT64_MAX for standalone test)
    auto a = alloc.Allocate(UINT64_MAX);
    auto b = alloc.Allocate(UINT64_MAX);
    auto c = alloc.Allocate(UINT64_MAX);

    if (!a.IsValid() || !b.IsValid() || !c.IsValid())
    {
        printf("FAIL: Allocate returned invalid handles\n");
        return false;
    }

    if (alloc.Used() != 3 || alloc.Capacity() != 4)
    {
        printf("FAIL: Used=%u (expected 3), Capacity=%u (expected 4)\n",
               alloc.Used(), alloc.Capacity());
        return false;
    }

    // Free one and re-allocate (reuses the freed slot)
    alloc.Free(b);
    if (alloc.Used() != 2)
    {
        printf("FAIL: After free, Used=%u (expected 2)\n", alloc.Used());
        return false;
    }

    auto d = alloc.Allocate(UINT64_MAX);
    if (!d.IsValid())
    {
        printf("FAIL: Re-allocate after free failed\n");
        return false;
    }

    if (alloc.Used() != 3)
    {
        printf("FAIL: After re-allocate, Used=%u (expected 3)\n", alloc.Used());
        return false;
    }

    // Trigger growth
    alloc.Allocate(UINT64_MAX); // fills slot 0..3
    auto e = alloc.Allocate(UINT64_MAX); // triggers Grow(4)

    if (!e.IsValid())
    {
        printf("FAIL: Grow allocation failed\n");
        return false;
    }

    if (alloc.Capacity() != 8)
    {
        printf("FAIL: After grow, Capacity=%u (expected 8)\n", alloc.Capacity());
        return false;
    }

    if (alloc.Used() != 5)
    {
        printf("FAIL: After grow+alloc, Used=%u (expected 5)\n", alloc.Used());
        return false;
    }

    // Stage() should not crash; UINT64_MAX releases all pending GPU heaps.
    alloc.Stage(UINT64_MAX);

    // Cleanup (Destroy called by destructor)
    printf("PASS: StagedDescriptorAllocator smoke test ok (cap=%u, used=%u)\n",
           alloc.Capacity(), alloc.Used());

    return true;
}

// Test contiguous block allocation.
static bool RunContiguousAllocTest(ID3D12Device* device)
{
    StagedDescriptorAllocator alloc;
    alloc.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8);

    // AllocContiguous(1) should work (edge case from review-3).
    auto single = alloc.AllocContiguous(1, UINT64_MAX);
    if (!single.IsValid())
    {
        printf("FAIL: AllocContiguous(1) returned invalid handle\n");
        return false;
    }

    // Allocate a block of 3 contiguous slots.
    auto block = alloc.AllocContiguous(3, UINT64_MAX);
    if (!block.IsValid())
    {
        printf("FAIL: AllocContiguous returned invalid handle\n");
        return false;
    }

    if (alloc.Used() != 3 || alloc.Capacity() != 8)
    {
        printf("FAIL: Contiguous alloc Used=%u (expected 3), Cap=%u (expected 8)\n",
               alloc.Used(), alloc.Capacity());
        return false;
    }

    // Verify the slots (including the single slot) are consecutive.
    UINT inc = alloc.DescriptorIncrement();
    D3D12_CPU_DESCRIPTOR_HANDLE base3 = alloc.CpuHandle(block.Index);
    for (UINT i = 1; i < 3; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE slot = alloc.CpuHandle(block.Index + i);
        if (slot.ptr != base3.ptr + i * inc)
        {
            printf("FAIL: Contiguous block slot %u not at expected offset "
                   "(expected %llu, got %llu)\n",
                   i, (unsigned long long)(base3.ptr + i * inc), (unsigned long long)slot.ptr);
            return false;
        }
    }

    // Verify that a new Allocate() does not return any of the contiguous slots.
    for (UINT i = 0; i < 5; ++i)
    {
        auto h = alloc.Allocate(UINT64_MAX);
        if (!h.IsValid())
        {
            printf("FAIL: Could not allocate after contiguous block\n");
            return false;
        }
        // All 5 remaining slots should be distinct from the contiguous block.
        for (UINT j = 0; j < 3; ++j)
        {
            if (h.Index == block.Index + j)
            {
                printf("FAIL: Allocate returned a slot (%u) from the contiguous block\n",
                       h.Index);
                return false;
            }
        }
    }

    // Free the contiguous block via FreeContiguous and verify the slots
    // go back to the free list.
    alloc.FreeContiguous(block, 3);

    // The freed contiguous range should now be reusable by a single AllocContiguous.
    auto block3 = alloc.AllocContiguous(3, UINT64_MAX);
    if (!block3.IsValid())
    {
        printf("FAIL: Re-allocation of freed contiguous block failed\n");
        return false;
    }

    alloc.Stage(UINT64_MAX);

    printf("PASS: ContiguousAlloc test ok\n");
    return true;
}

// Entry point called from D3D12HelloTexture after device init (debug builds only).
void RunStagedAllocatorTests(ID3D12Device* device)
{
    RunStagedAllocatorTest(device);
    RunContiguousAllocTest(device);
}
