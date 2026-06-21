# feature/growable-descriptor-heap

## 目的

現在の `kMainHeapDescriptorCount` 手動計算方式を脱却し、
必要に応じて自動で増加する growable descriptor heap を導入する。

## 背景と問題

現状:
- 単一の shader-visible descriptor heap を `kMainHeapDescriptorCount` で固定確保
- 新しい descriptor を追加するたびにカウントを手動で更新する必要がある
- カウントミスは実行時 assert またはクラッシュにつながる

## 理想形

```
CPU heap (D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 非表示)
  └── アプリケーションが自由に Alloc/Free
  └── 格納されている descriptor は常に最新

GPU heap (D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, shader-visible)
  └── 毎フレーム、CPU heap の内容を CopyDescriptors で一括コピー
  └── コピー元の範囲は [0, 最大使用スロット数)
```

### なぜ複雑でないか

- `CopyDescriptors` は1回のAPI呼び出しで GPU heap を更新できる
- 「全部コピー」する場合、dirty 追跡は不要（全部コピーするだけ）
- ディスクリプタ数が数千でもコピーコストは数十μs 程度

### 本当に難しい部分

1. **Transient descriptor のライフタイム管理**
   - 中間レンダーパス用の descriptor はフレームごとに確保・解放される
   - GPU 実行完了まで解放できない
   - 「いつ再利用可能か」の追跡には Fence ベースの管理が必要
   - リングバッファ方式は最大フレーム数を超えると破綻する

2. **設計判断**
   - 全コピー vs dirty 追跡
   - Permanent と Transient の heap 分離 vs 統一
   - RAII ハンドルの型設計

## 進め方

1. 列挙型による slot カウント自動化（中間段階）← 実施済み
2. CPU 非表示 heap + 毎フレーム staging の試作
3. Transient descriptor のライフタイム管理の設計と実装

## 実施内容

### Step 1: `PersistentSrvSlot` 列挙型の導入

`D3D12HelloTexture.h` に `PersistentSrvSlot` 列挙型を追加。

```cpp
enum PersistentSrvSlot : UINT
{
    DepthStencilSrvSlot,
    LightPassColorSrvSlot,

    PersistentSrvSlotCount,
};
```

`kMainHeapDescriptorCount` の末尾の `+ 2` を `+ PersistentSrvSlotCount` に変更。
新しい固定 SRV スロットを追加する場合は、列挙子を1行追加するだけでカウントが自動反映される。
デスクリプタの確保順が enum の定義順と一致することを前提としている（現在の実装と変わらない）。

### Step 2: `StagedDescriptorAllocator` プロトタイプ

`Renderer/StagedDescriptorAllocator.h` に実装。

#### 設計

2つのヒープを管理:
- **CPU heap** (`D3D12_DESCRIPTOR_HEAP_FLAG_NONE`): 常に最新の記述子を保持。全 Alloc/Free/Write はここに。
- **GPU heap** (`D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`): 毎フレーム `CopyDescriptorsSimple` で CPU から全コピー。

```cpp
StagedDescriptorHandle Allocate();             // 1 slot 確保
StagedDescriptorHandle AllocContiguous(count); // count 連続 slot 確保
void Stage(completedFenceValue);               // CPU → GPU コピー + 古い GPU heap 解放
void Free(StagedDescriptorHandle);             // slot を解放
void SetPendingFenceValue(value);              // 次回 Grow に備えて fence value を記録
```

#### Grow 動作

空きがない場合、自動的に Grow する:
1. 現在の容量 + N の新しい CPU heap / GPU heap を作成
2. 既存の記述子を古い CPU heap から新しい CPU heap へコピー
3. 新しい GPU heap にも同様にコピー
4. 新しい空きスロットを FreeIndices に追加
5. 古い GPU heap を pending list に移行（即時解放しない）
6. `Stage(completedFenceValue)` で fence 完了後に pending list から解放

#### Review 対応

Review ([review-1](growable-descriptor-heap-review-1-cc4f63fed3b3.md)) / ([review-2](growable-descriptor-heap-review-2-a6cbc0a383ea.md)) 指摘に基づく修正:

##### 1. `StagedDescriptorHandle` を slot-only に変更

従来は handle が CPU/GPU 絶対アドレスをキャッシュしていたが、Grow 後に stale になるため廃止。
Handle は `Index` のみ保持し、CPU/GPU handle は `CpuHandle(slot)` / `GpuHandle(slot)` accessor で都度解決する。

```cpp
StagedDescriptorHandle h = alloc.Allocate();
D3D12_CPU_DESCRIPTOR_HANDLE cpu = alloc.CpuHandle(h.Index);
D3D12_GPU_DESCRIPTOR_HANDLE gpu = alloc.GpuHandle(h.Index);
```

##### 2. 古い GPU heap の deferred release

`Grow()` で旧 GPU heap を `m_pendingGpuHeaps` に fence value 付きで退避。
`Stage(completedFenceValue)` 呼び出し時に fence 完了済みの pending heap を解放。
`SetPendingFenceValue(value)` で次回 Grow に使う fence value を事前登録する。

##### 3. Smoke test を `_DEBUG` 限定に移行

`InitializeFrameResources()` 内の呼び出しを `#if defined(_DEBUG)` でガード。
Release ビルドではテストは実行されない。

##### 4. `AllocContiguous(count)` の追加

連続 `count` 個の descriptor slot を1回の呼び出しで確保（descriptor table 用）。
内部で free list をソートして連続領域を探索。見つからなければ Grow してから再試行。

#### Smoke test

`Renderer/StagedDescriptorAllocator_Test.cpp` に単体テストを実装。
Debug ビルド時に起動後に1回実行。
テスト内容:
- 確保・解放・再利用・Grow・Stage の基本動作
- `AllocContiguous()` の連続確保と断片化後の動作
