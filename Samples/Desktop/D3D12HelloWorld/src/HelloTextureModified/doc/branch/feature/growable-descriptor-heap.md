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
StagedDescriptorHandle Allocate();  // 1 slot 確保
void Stage();                        // 毎フレーム1回: CPU → GPU コピー
void Free(StagedDescriptorHandle);   // slot を解放
```

#### Grow 動作

空きがない場合、自動的に Grow する:
1. 現在の容量 + N の新しい CPU heap / GPU heap を作成
2. 既存の記述子を古い CPU heap から新しい CPU heap へコピー
3. 新しい GPU heap にも同様にコピー
4. 新しい空きスロットを FreeIndices に追加
5. 古いヒープを解放

#### Smoke test

`Renderer/StagedDescriptorAllocator_Test.cpp` に単体テストを実装。
`InitializeFrameResources()` 内で起動時に1回実行。
テスト内容: 確保・解放・再利用・Grow・Stage を検証。
