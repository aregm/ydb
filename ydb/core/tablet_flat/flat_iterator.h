#pragma once
#include "flat_iterator_ops.h"
#include "flat_mem_iter.h"
#include "flat_part_iter_multi.h"
#include "flat_row_remap.h"
#include "flat_row_state.h"
#include "flat_range_cache.h"
#include "util_fmt_cell.h"

#include <library/cpp/containers/stack_vector/stack_vec.h>

#include <util/draft/holder_vector.h>
#include <util/generic/vector.h>
#include <util/generic/queue.h>

namespace NKikimr {
namespace NTable {

enum class ENext {
    All,
    Data,
    Uncommitted,
};

struct TIteratorStats {
    ui64 DeletedRowSkips = 0;
    ui64 InvisibleRowSkips = 0;
};

template<class TIteratorOps>
class TTableItBase : TNonCopyable {
    enum class EType : ui8 {
        Mem = 0,
        Run = 1,
        Stop = 2,
    };

    enum class EStage : ui8 {
        Seek    = 0,    /* Have Stuck on Seek() stage    */
        Done    = 1,
        Turn    = 2,
        Snap    = 3,
        Fill    = 4,
    };

    class TEraseCachingState {
        using TOps = typename TIteratorOps::TEraseCacheOps;

    public:
        TEraseCachingState(TTableItBase* it)
            : It(it)
            , Cache(it->ErasedKeysCache.Get())
        { }

        ~TEraseCachingState() {
            if (!Cache) {
                return;
            }

            FreeKey(&LastErased);
            FreeKey(&FirstErased);
        }

        void OnEraseKey(TArrayRef<const TCell> key, TRowVersion version) {
            if (!Cache)
                return;

            if (FutureEntryValid && Cache.CrossedAtRight(FutureEntry, key)) {
                // The iterator is still in future range, will skip all checks
                return;
            }

            FutureEntryValid = false;

            if (NextEntryValid && Cache.CrossedAtLeft(NextEntry, key)) {
                // We have just crossed the gap into the next entry
                FreeKey(&LastErased);
                if (PrevEntryValid) {
                    // Merge the two adjacent entries that we have
                    Y_VERIFY_DEBUG(!FirstErased);
                    Y_VERIFY_DEBUG(MaxVersion >= PrevEntry->MaxVersion);
                    NextEntry = Cache.MergeAdjacent(PrevEntry, NextEntry, ::Max(MaxVersion, version));
                } else if (FirstErased) {
                    // Extend the next entry with an earlier key
                    Cache.ExtendBackward(NextEntry, TakeKey(&FirstErased), true, ::Max(MaxVersion, version));
                }
                Cache.Touch(NextEntry);
                Cache.EvictOld();

                PrevEntryValid = false;
                NextEntryValid = false;
                ErasedCount = 0;
                MaxVersion = TRowVersion::Min();

                Y_VERIFY_DEBUG(!FirstErased);
                Y_VERIFY_DEBUG(!LastErased);

                if (NextEntry->MaxVersion > It->SnapshotVersion) {
                    // The range is in future, the iterator cannot use it
                    FutureEntry = NextEntry;
                    FutureEntryValid = true;
                    return;
                }

                if (!TOps::EndKey(NextEntry)) {
                    // We know that everything to +inf is erased
                    It->Iterators.clear();
                    It->Active = It->Iterators.end();
                    It->Ready = EReady::Gone;
                    return;
                }

                if (!It->SkipTo(TOps::EndKey(NextEntry), !TOps::EndInclusive(NextEntry))) {
                    // We've got some missing page, cannot iterate further
                    return;
                }

                // We just jumped past the next entry, it becomes the prev entry
                PrevEntry = NextEntry;
                PrevEntryValid = true;
                NextEntry = Cache.Next(PrevEntry);
                NextEntryValid = true;
                MaxVersion = PrevEntry->MaxVersion;
                return;
            }

            if (PrevEntryValid || FirstErased) {
                FreeKey(&LastErased);
                LastErased = Cache.AllocateKey(key);
                MaxVersion = ::Max(MaxVersion, version);
                ++ErasedCount;
                return;
            }

            Y_VERIFY_DEBUG(ErasedCount == 0);

            if (NextEntryValid) {
                // We already know the next erased key, but we don't know
                // the first key, so we are not caching this range
                return;
            }

            Y_VERIFY_DEBUG(!PrevEntryValid);
            Y_VERIFY_DEBUG(!NextEntryValid);

            // Check if this key is an already known erase key
            auto res = Cache.FindKey(key);
            if (res.second) {
                if (res.first->MaxVersion > It->SnapshotVersion) {
                    // The range is in future, the iterator cannot use it
                    FutureEntry = res.first;
                    FutureEntryValid = true;
                    return;
                }

                Cache.Touch(res.first);
                if (!It->SkipTo(TOps::EndKey(res.first), !TOps::EndInclusive(res.first))) {
                    // We've got some missing page, cannot iterate further
                    return;
                }
                PrevEntry = res.first;
                PrevEntryValid = true;
                NextEntry = Cache.Next(PrevEntry);
                NextEntryValid = true;
                MaxVersion = PrevEntry->MaxVersion;
                return;
            }

            NextEntry = res.first;
            NextEntryValid = true;
            if (!Cache.CachingAllowed()) {
                // We are over the limit, don't start anything new
                return;
            }

            FirstErased = Cache.AllocateKey(key);
            MaxVersion = version;
            ++ErasedCount;
        }

        void Flush() {
            if (!Cache) {
                return;
            }

            if (ErasedCount > 0 && LastErased) {
                if (PrevEntryValid) {
                    Y_VERIFY_DEBUG(!FirstErased);
                    Cache.ExtendForward(PrevEntry, TakeKey(&LastErased), true, MaxVersion);
                    Cache.EvictOld();
                } else if (ErasedCount >= Cache.MinRows()) {
                    // We have enough erased rows to make a new cache entry
                    Y_VERIFY_DEBUG(FirstErased);
                    Cache.AddRange(TakeKey(&FirstErased), TakeKey(&LastErased), MaxVersion);
                    Cache.EvictOld();
                }
            }

            FreeKey(&LastErased);
            FreeKey(&FirstErased);
            PrevEntryValid = false;
            NextEntryValid = false;
            FutureEntryValid = false;
            ErasedCount = 0;
            MaxVersion = TRowVersion::Min();
        }

    private:
        void FreeKey(TArrayRef<TCell>* key) {
            if (*key) {
                Cache.DeallocateKey(*key);
                *key = { };
            }
        }

        TArrayRef<TCell> TakeKey(TArrayRef<TCell>* key) {
            auto ref = *key;
            *key = { };
            return ref;
        }

    private:
        TTableItBase* It;
        TOps Cache;
        TKeyRangeCache::const_iterator PrevEntry;
        TKeyRangeCache::const_iterator NextEntry;
        TKeyRangeCache::const_iterator FutureEntry;
        TArrayRef<TCell> FirstErased;
        TArrayRef<TCell> LastErased;
        size_t ErasedCount = 0;
        TRowVersion MaxVersion = TRowVersion::Min();
        bool PrevEntryValid = false;
        bool NextEntryValid = false;
        bool FutureEntryValid = false;
    };

public:
    TTableItBase(
        const TRowScheme* scheme, TTagsRef tags, ui64 lim = Max<ui64>(),
        TRowVersion snapshot = TRowVersion::Max(),
        const NTable::TTransactionMap<TRowVersion>& committedTransactions = {});

    ~TTableItBase();

    void Push(TAutoPtr<TMemIt>);
    void Push(TAutoPtr<TRunIt>);

    void StopBefore(TArrayRef<const TCell> key);
    void StopAfter(TArrayRef<const TCell> key);

    bool SkipTo(TCelled::TRaw key, bool inclusive = true) noexcept {
        TCelled cells(key, *Scheme->Keys, false);

        return SkipTo(cells, inclusive);
    }

    bool SkipTo(TArrayRef<const TCell> key, bool inclusive = true) noexcept {
        return SeekInternal(key, inclusive ? ESeek::Lower : ESeek::Upper);
    }

    EReady Next(ENext mode) noexcept
    {
        TEraseCachingState eraseCache(this);

        bool isHead = true;
        for (Ready = EReady::Data; Ready == EReady::Data; ) {
            if (Stage == EStage::Seek) {
                Ready = Start();
            } else if (Stage == EStage::Done) {
                Stage = EStage::Turn;
                Ready = Turn();
            } else if (Stage == EStage::Turn) {
                Ready = Turn();
            } else if (Stage == EStage::Snap) {
                if (mode != ENext::Uncommitted) {
                    ui64 skipsBefore = Stats.InvisibleRowSkips;
                    Ready = Snap();
                    isHead = skipsBefore == Stats.InvisibleRowSkips;
                } else {
                    Y_VERIFY_DEBUG(Active != Inactive);
                    Stage = EStage::Fill;
                    isHead = false;
                }
            } else if ((Ready = Apply()) != EReady::Data) {

            } else if (mode == ENext::All || mode == ENext::Uncommitted || State.GetRowState() != ERowOp::Erase) {
                break;
            } else {
                ++Stats.DeletedRowSkips; /* skip internal technical row states w/o data */
                if (ErasedKeysCache) {
                    if (isHead) {
                        eraseCache.OnEraseKey(GetKey().Cells(), GetRowVersion());
                    } else {
                        eraseCache.Flush();
                        isHead = true;
                    }
                }
            }
        }

        if (ErasedKeysCache && mode != ENext::All) {
            eraseCache.Flush();
        }

        return Last();
    }

    EReady Last() const noexcept
    {
        return Ready;
    }

    TDbTupleRef GetKey() const noexcept;
    TDbTupleRef GetValues() const noexcept;

    const TRowState& Row() const noexcept
    {
        return State;
    }

    bool IsUncommitted() const noexcept;
    ui64 GetUncommittedTxId() const noexcept;
    EReady SkipUncommitted() noexcept;
    TRowVersion GetRowVersion() const noexcept;
    EReady SkipToRowVersion(TRowVersion rowVersion) noexcept;

public:
    const TRowScheme* Scheme;
    const TRemap Remap;
    TIntrusivePtr<TKeyRangeCache> ErasedKeysCache;
    TIteratorStats Stats;

private:
    ui64 Limit = 0;

    TRowState State;

    // RowVersion of a persistent snapshot that we are reading
    // By default iterator is initialized with the HEAD snapshot
    const TRowVersion SnapshotVersion;

    // A map of currently committed transactions to corresponding row versions
    const NTable::TTransactionMap<TRowVersion> CommittedTransactions;

    EStage Stage = EStage::Seek;
    EReady Ready = EReady::Gone;
    THolderVector<TMemIt> MemIters;
    THolderVector<TRunIt> RunIters;
    TOwnedCellVec StopKey;
    bool StopKeyInclusive = true;

    struct TIteratorId {
        EType Type;
        ui16 Index;
        TEpoch Epoch;
    };

    struct TElement {
        TArrayRef<const TCell> Key;
        TIteratorId IteratorId;
    };

    struct TComparator {
        TComparator(TArrayRef<const NScheme::TTypeIdOrder> types)
            : Types(types)
        {}

        int CompareKeys(TArrayRef<const TCell> a, TArrayRef<const TCell> b) const noexcept
        {
            return TIteratorOps::CompareKeys(Types, a, b);
        }

        bool operator() (const TElement& a, const TElement& b) const noexcept
        {
            if (int cmp = CompareKeys(a.Key, b.Key))
                return cmp > 0;

            Y_VERIFY_DEBUG(a.IteratorId.Epoch != b.IteratorId.Epoch,
                "Found equal key iterators with the same epoch");
            return a.IteratorId.Epoch < b.IteratorId.Epoch;
        }

        const TArrayRef<const NScheme::TTypeIdOrder> Types;
    };

    /**
     * Adjust epoch into a modified range
     *
     * This frees epoch=-inf and epoch=+inf for special stop keys. Note that this
     * would convert +inf into +inf, which should be safe, since epoch=+inf is
     * normally used as an invalid epoch marker.
     */
    static TEpoch AdjustEpoch(TEpoch epoch) {
        if (epoch == TEpoch::Max()) {
            return TEpoch::Max(); // invalid epoch
        } else {
            return ++epoch;
        }
    }

    void Clear() {
        Iterators.clear();
        Active = Iterators.end();
        Inactive = Active;
    }

    // ITERATORS STORAGE
    // Range [ IteratorsHeap.begin(); Active ) form a heap
    // Range [ Active; IteratorsHeap.end() ) are currently active iterators (i.e. that have key equal to current key)
    // Also note that active iterators should be traversed in reverse order
    TComparator Comparator;
    using TIterators = TSmallVec<TElement>;
    using TForwardIter = typename TIterators::iterator;
    using TReverseIter = typename TIterators::reverse_iterator;

    TIterators Iterators;
    TForwardIter Active;
    TForwardIter Inactive;
    ui64 DeltaTxId = 0;
    TRowVersion DeltaVersion;
    bool Delta = false;
    bool Uncommitted = false;

    EReady Start() noexcept;
    EReady Turn() noexcept;
    EReady Snap() noexcept;
    EReady Snap(TRowVersion rowVersion) noexcept;
    EReady DoSkipUncommitted() noexcept;
    EReady Apply() noexcept;
    void AddReadyIterator(TArrayRef<const TCell> key, TIteratorId itId);
    void AddNotReadyIterator(TIteratorId itId);

    bool SeekInternal(TArrayRef<const TCell> key, ESeek seek) noexcept;
};

class TTableIt;
class TTableReverseIt;

class TTableIt : public TTableItBase<TTableItOps> {
public:
    using TTableItBase::TTableItBase;

    typedef TTableReverseIt TReverseType;

    static constexpr EDirection Direction = EDirection::Forward;
};

class TTableReverseIt : public TTableItBase<TTableItReverseOps> {
public:
    using TTableItBase::TTableItBase;

    typedef TTableIt TReverseType;

    static constexpr EDirection Direction = EDirection::Reverse;
};

template<class TIteratorOps>
inline TTableItBase<TIteratorOps>::TTableItBase(
        const TRowScheme* scheme, TTagsRef tags, ui64 limit,
        TRowVersion snapshot,
        const NTable::TTransactionMap<TRowVersion>& committedTransactions)
    : Scheme(scheme)
    , Remap(*Scheme, tags)
    , Limit(limit)
    , State(Remap.Size())
    , SnapshotVersion(snapshot)
    , CommittedTransactions(committedTransactions)
    , Comparator(Scheme->Keys->Types)
    , Active(Iterators.end())
    , Inactive(Iterators.end())
{}

template<class TIteratorOps>
inline TTableItBase<TIteratorOps>::~TTableItBase()
{
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::AddReadyIterator(TArrayRef<const TCell> key, TIteratorId itId) {
    Active = Iterators.emplace(Active, TElement({key, itId}));
    ++Active;
    std::push_heap(Iterators.begin(), Active, Comparator);
    Inactive = Active;
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::AddNotReadyIterator(TIteratorId itId) {
    size_t actPos = Active - Iterators.begin();
    Iterators.emplace_back(TElement({{ }, itId}));
    Active = Iterators.begin() + actPos;
    Inactive = Active;
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::Push(TAutoPtr<TMemIt> it)
{
    if (it && it->IsValid()) {
        TIteratorId itId = { EType::Mem, ui16(MemIters.size()), AdjustEpoch(it->MemTable->Epoch) };

        MemIters.PushBack(it);
        TDbTupleRef key = MemIters.back()->GetKey();
        AddReadyIterator(key.Cells(), itId);
    }
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::Push(TAutoPtr<TRunIt> it)
{
    TIteratorId itId = { EType::Run, ui16(RunIters.size()), AdjustEpoch(it->Epoch()) };

    bool ready = it->IsValid();

    RunIters.PushBack(std::move(it));
    if (ready) {
        TDbTupleRef key = RunIters.back()->GetKey();
        AddReadyIterator(key.Cells(), itId);
    } else {
        AddNotReadyIterator(itId);
    }
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::StopBefore(TArrayRef<const TCell> key)
{
    Y_VERIFY(!StopKey, "Using multiple stop keys not allowed");

    if (Y_UNLIKELY(!key)) {
        return;
    }

    StopKey = TOwnedCellVec::Make(key);
    StopKeyInclusive = false;

    TIteratorId itId = { EType::Stop, Max<ui16>(), TEpoch::Max() };

    AddReadyIterator(StopKey, itId);
}

template<class TIteratorOps>
inline void TTableItBase<TIteratorOps>::StopAfter(TArrayRef<const TCell> key)
{
    Y_VERIFY(!StopKey, "Using multiple stop keys not allowed");

    if (Y_UNLIKELY(!key)) {
        return;
    }

    StopKey = TOwnedCellVec::Make(key);
    StopKeyInclusive = true;

    TIteratorId itId = { EType::Stop, Max<ui16>(), TEpoch::Min() };

    AddReadyIterator(StopKey, itId);
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::Start() noexcept
{
    if (Active != Iterators.end()) {
        return EReady::Page;
    }

    if (!Iterators ||
        Iterators.front().IteratorId.Type == EType::Stop ||
        !(Limit && Limit--))
    {
        return EReady::Gone;
    }

    auto key = Iterators.front().Key;
    PopHeap(Iterators.begin(), Active--, Comparator);
    while (Active != Iterators.begin()) {
        if (Comparator.CompareKeys(key, Iterators.front().Key) != 0)
            break;

        if (Iterators.front().IteratorId.Type == EType::Stop) {
            // This is the last row we may return
            Limit = 0;
            break;
        }

        PopHeap(Iterators.begin(), Active--, Comparator);
    }

    Stage = EStage::Snap;
    Inactive = Iterators.end();
    return EReady::Data;
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::Turn() noexcept
{
    if (!Limit) {
        // Optimization: avoid calling Next after returning the last row
        return EReady::Gone;
    }

    bool ready = true;
    size_t left = Iterators.end() - Active;
    while (left-- > 0) {
        TIteratorId ai = Active->IteratorId;
        ui32 idx = ai.Index;
        switch (ai.Type) {
            case EType::Mem: {
                auto& mi = *MemIters[idx];
                TIteratorOps::MoveNext(mi);
                if (mi.IsValid()) {
                    Active->Key = mi.GetKey().Cells();
                    Y_VERIFY_DEBUG(Active->Key.size() == Scheme->Keys->Types.size());
                    std::push_heap(Iterators.begin(), ++Active, Comparator);
                } else {
                    Active = Iterators.erase(Active);
                    Inactive = Active;
                }
                break;
            }
            case EType::Run: {
                auto& it = *RunIters[idx];
                switch (TIteratorOps::MoveNext(it)) {
                    case EReady::Page:
                        if (left > 0) {
                            std::swap(*Active, *(Active + left));
                        }
                        ready = false;
                        break;

                    case EReady::Data:
                        Active->Key = it.GetKey().Cells();
                        Y_VERIFY_DEBUG(Active->Key.size() == Scheme->Keys->Types.size());
                        Active->IteratorId.Epoch = AdjustEpoch(it.Epoch());
                        std::push_heap(Iterators.begin(), ++Active, Comparator);
                        break;

                    case EReady::Gone:
                        Active = Iterators.erase(Active);
                        Inactive = Active;
                        break;

                    default:
                        Y_FAIL("Unexpected EReady value");
                }
                break;
            }
            default: {
                Y_FAIL("Unexpected iterator type");
            }
        }
    }

    if (!ready) {
        return EReady::Page;
    }

    return Start();
}

template<class TIteratorOps>
inline bool TTableItBase<TIteratorOps>::IsUncommitted() const noexcept
{
    // Must only be called after a fully successful Apply()
    Y_VERIFY_DEBUG(Stage == EStage::Done && Ready == EReady::Data);

    // There must be at least one active iterator
    Y_VERIFY_DEBUG(Active != Inactive);

    return Delta && Uncommitted;
}

template<class TIteratorOps>
inline ui64 TTableItBase<TIteratorOps>::GetUncommittedTxId() const noexcept
{
    // Must only be called after a fully successful Apply()
    Y_VERIFY_DEBUG(Stage == EStage::Done && Ready == EReady::Data);

    // There must be at least one active iterator
    Y_VERIFY_DEBUG(Active != Inactive);

    // Must only be called for uncommitted positions
    Y_VERIFY_DEBUG(Delta && Uncommitted);

    return DeltaTxId;
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::SkipUncommitted() noexcept
{
    // Must only be called after successful Apply() or page fault in SkipUncommitted()
    Y_VERIFY_DEBUG(Stage == EStage::Done && Ready != EReady::Gone);

    if (Ready == EReady::Page) {
        // Previous SkipUncommitted() failed with a page fault, we must not skip again
        return Ready = Apply();
    }

    switch (DoSkipUncommitted()) {
        case EReady::Data:
            return Ready = Apply();

        case EReady::Gone:
            return Ready = EReady::Gone;

        case EReady::Page:
            return Ready = EReady::Page;
    }

    Y_UNREACHABLE();
}

template<class TIteratorOps>
inline TRowVersion TTableItBase<TIteratorOps>::GetRowVersion() const noexcept
{
    // Must only be called after a fully successful Apply()
    Y_VERIFY_DEBUG(Stage == EStage::Done && Ready == EReady::Data);

    // There must be at least one active iterator
    Y_VERIFY_DEBUG(Active != Inactive);

    if (Delta) {
        Y_VERIFY_DEBUG(!Uncommitted, "Cannot get version for uncommitted deltas");
        return DeltaVersion;
    }

    TIteratorId ai = TReverseIter(Inactive)->IteratorId;
    switch (ai.Type) {
        case EType::Mem: {
            return MemIters[ai.Index]->GetRowVersion();
        }
        case EType::Run: {
            return RunIters[ai.Index]->GetRowVersion();
        }
        default:
            Y_FAIL("Unexpected iterator type");
    }
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::SkipToRowVersion(TRowVersion rowVersion) noexcept
{
    // Must only be called after successful Apply()
    Y_VERIFY_DEBUG(Stage == EStage::Done && Ready != EReady::Gone);

    switch (Snap(rowVersion)) {
        case EReady::Data:
            return Ready = Apply();

        case EReady::Gone:
            return Ready = EReady::Gone;

        case EReady::Page:
            return Ready = EReady::Page;
    }

    Y_UNREACHABLE();
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::Snap() noexcept
{
    Y_VERIFY_DEBUG(Active != Inactive);

    switch (Snap(SnapshotVersion)) {
        case EReady::Data:
            Stage = EStage::Fill;
            return EReady::Data;

        case EReady::Gone:
            Stage = EStage::Turn;
            return EReady::Data;

        case EReady::Page:
            return EReady::Page;
    }

    Y_UNREACHABLE();
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::Snap(TRowVersion rowVersion) noexcept
{
    for (auto i = TReverseIter(Inactive), e = TReverseIter(Active); i != e; ++i) {
        TIteratorId ai = i->IteratorId;
        switch (ai.Type) {
            case EType::Mem: {
                auto ready = MemIters[ai.Index]->SkipToRowVersion(rowVersion, CommittedTransactions);
                Stats.InvisibleRowSkips += std::exchange(MemIters[ai.Index]->InvisibleRowSkips, 0);
                if (ready) {
                    return EReady::Data;
                }
                break;
            }
            case EType::Run: {
                auto ready = RunIters[ai.Index]->SkipToRowVersion(rowVersion, CommittedTransactions);
                Stats.InvisibleRowSkips += std::exchange(RunIters[ai.Index]->InvisibleRowSkips, 0);
                if (ready == EReady::Data) {
                    return EReady::Data;
                } else if (ready != EReady::Gone) {
                    Y_VERIFY_DEBUG(ready == EReady::Page);
                    return ready;
                }
                break;
            }
            default:
                Y_FAIL("Unexpected iterator type");
        }

        // The last iterator becomes inactive
        --Inactive;
    }

    // We ran out of versions, move to the next key
    Y_VERIFY_DEBUG(Active == Inactive);
    return EReady::Gone;
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::DoSkipUncommitted() noexcept
{
    Y_VERIFY_DEBUG(Delta && Uncommitted);

    for (auto i = TReverseIter(Inactive), e = TReverseIter(Active); i != e; ++i) {
        TIteratorId ai = i->IteratorId;
        switch (ai.Type) {
            case EType::Mem: {
                auto& it = *MemIters[ai.Index];
                Y_VERIFY_DEBUG(it.IsDelta() && !CommittedTransactions.Find(it.GetDeltaTxId()));
                if (it.SkipDelta()) {
                    return EReady::Data;
                }
                break;
            }
            case EType::Run: {
                auto& it = *RunIters[ai.Index];
                Y_VERIFY_DEBUG(it.IsDelta() && !CommittedTransactions.Find(it.GetDeltaTxId()));
                auto ready = it.SkipDelta();
                if (ready != EReady::Gone) {
                    return ready;
                }
                break;
            }
            default:
                Y_FAIL("Unexpected iterator type");
        }

        // The last iterator becomes inactive
        --Inactive;
        break;
    }

    if (Active != Inactive) {
        return EReady::Data;
    } else {
        return EReady::Gone;
    }
}

template<class TIteratorOps>
inline EReady TTableItBase<TIteratorOps>::Apply() noexcept
{
    State.Reset(Remap.CellDefaults());

    const TDbTupleRef key = GetKey();

    for (auto &pin: Remap.KeyPins())
        State.Set(pin.Pos, { ECellOp::Set, ELargeObj::Inline }, key.Columns[pin.Key]);

    // We must have at least one active iterator
    Y_VERIFY_DEBUG(Active != Inactive);

    bool committed = false;
    for (auto i = TReverseIter(Inactive), e = TReverseIter(Active); i != e; ++i) {
        TIteratorId ai = i->IteratorId;
        switch (ai.Type) {
            case EType::Mem: {
                auto& it = *MemIters[ai.Index];
                if (!committed) {
                    Delta = it.IsDelta();
                    if (Delta) {
                        DeltaTxId = it.GetDeltaTxId();
                        const TRowVersion* rowVersion = CommittedTransactions.Find(DeltaTxId);
                        if (!rowVersion) {
                            it.ApplyDelta(State);
                            Uncommitted = true;
                            break;
                        }
                        DeltaVersion = *rowVersion;
                    }
                    Uncommitted = false;
                    committed = true;
                }
                it.Apply(State, CommittedTransactions);
                break;
            }
            case EType::Run: {
                auto& it = *RunIters[ai.Index];
                if (!committed) {
                    Delta = it.IsDelta();
                    if (Delta) {
                        DeltaTxId = it.GetDeltaTxId();
                        const TRowVersion* rowVersion = CommittedTransactions.Find(DeltaTxId);
                        if (!rowVersion) {
                            it.ApplyDelta(State);
                            Uncommitted = true;
                            break;
                        }
                        DeltaVersion = *rowVersion;
                    }
                    Uncommitted = false;
                    committed = true;
                }
                it.Apply(State, CommittedTransactions);
                break;
            }
            default:
                Y_FAIL("Unexpected iterator type");
        }

        if (State.IsFinalized() || !committed)
            break;
    }

    if (State.Need()) {
        return EReady::Page;
    }

    Stage = EStage::Done;
    return EReady::Data;
}

template<class TIteratorOps>
inline TDbTupleRef TTableItBase<TIteratorOps>::GetKey() const noexcept
{
    TIteratorId ai = Iterators.back().IteratorId;
    switch (ai.Type) {
        case EType::Mem:
            return MemIters[ai.Index]->GetKey();
        case EType::Run:
            return RunIters[ai.Index]->GetKey();
        default:
            Y_FAIL("Unexpected iterator type");
    }
}

template<class TIteratorOps>
inline TDbTupleRef TTableItBase<TIteratorOps>::GetValues() const noexcept
{
    if (State.GetRowState() == ERowOp::Erase)
        return TDbTupleRef();
    return TDbTupleRef(Remap.Types().begin(), (*State).begin(), (*State).size());
}

template<class TIteratorOps>
inline bool TTableItBase<TIteratorOps>::SeekInternal(TArrayRef<const TCell> key, ESeek seek) noexcept
{
    Stage = EStage::Seek;

    if (!Limit || Y_UNLIKELY(!key)) {
        // Optimization: avoid calling Seek after returning the last row
        Clear();
        Ready = EReady::Gone;
        return true;
    }

    while (Active != Iterators.begin()) {
        int cmp = Comparator.CompareKeys(key, Iterators.front().Key);
        if (cmp < 0) {
            break;
        }
        if (Iterators.front().IteratorId.Type == EType::Stop) {
            if (cmp > 0 || seek == ESeek::Upper || Iterators.front().IteratorId.Epoch == TEpoch::Max()) {
                // Seek past the last row, Next must return Gone
                Clear();
                Ready = EReady::Gone;
                return true;
            }
            break;
        }
        if (cmp == 0 && seek != ESeek::Upper) {
            break;
        }
        PopHeap(Iterators.begin(), Active--, Comparator);
    }

    bool ready = true;
    size_t left = Iterators.end() - Active;
    while (left-- > 0) {
        TIteratorId ai = Active->IteratorId;
        ui32 idx = ai.Index;
        switch (ai.Type) {
            case EType::Mem: {
                auto& mi = *MemIters[idx];
                TIteratorOps::Seek(mi, key, seek);
                if (mi.IsValid()) {
                    Active->Key = mi.GetKey().Cells();
                    Y_VERIFY_DEBUG(Active->Key.size() == Scheme->Keys->Types.size());
                    std::push_heap(Iterators.begin(), ++Active, Comparator);
                } else {
                    Active = Iterators.erase(Active);
                }
                break;
            }
            case EType::Run: {
                auto& it = *RunIters[idx];
                switch (TIteratorOps::Seek(it, key, seek)) {
                    case EReady::Page:
                        if (left > 0) {
                            std::swap(*Active, *(Active + left));
                        }
                        ready = false;
                        break;

                    case EReady::Data:
                        Active->Key = it.GetKey().Cells();
                        Y_VERIFY_DEBUG(Active->Key.size() == Scheme->Keys->Types.size());
                        Active->IteratorId.Epoch = AdjustEpoch(it.Epoch());
                        std::push_heap(Iterators.begin(), ++Active, Comparator);
                        break;

                    case EReady::Gone:
                        Active = Iterators.erase(Active);
                        break;

                    default:
                        Y_FAIL("Unexpected EReady value");
                }
                break;
            }
            default: {
                Y_FAIL("Unexpected iterator type");
            }
        }
    }

    if (!ready) {
        Ready = EReady::Page;
    }

    return ready;
}

}
}
