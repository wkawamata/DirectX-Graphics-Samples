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

    // Allocate 3 slots
    auto a = alloc.Allocate();
    auto b = alloc.Allocate();
    auto c = alloc.Allocate();

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

    auto d = alloc.Allocate();
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
    alloc.Allocate(); // fills slot 0..3
    auto e = alloc.Allocate(); // triggers Grow(4)

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

// Entry point called from D3D12HelloTexture after device init.
void RunStagedAllocatorTests(ID3D12Device* device)
{
    RunStagedAllocatorTest(device);
}
