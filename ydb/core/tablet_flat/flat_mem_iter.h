#pragma once

#include "flat_update_op.h"
#include "flat_row_scheme.h"
#include "flat_row_remap.h"
#include "flat_row_state.h"
#include "flat_mem_warm.h"
#include "flat_mem_snapshot.h"
#include "flat_part_iface.h"
#include "flat_page_label.h"
#include "flat_table_committed.h"
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/scheme/scheme_type_id.h>

namespace NKikimr {
namespace NTable {

    class TMemIt {
    public:
        using TCells = TArrayRef<const TCell>;

        TMemIt(const TMemTable* memTable,
                TIntrusiveConstPtr<TKeyCellDefaults> keyDefaults,
                const TRemap* remap,
                IPages *env,
                NMem::TTreeIterator iterator)
            : MemTable(memTable)
            , KeyCellDefaults(std::move(keyDefaults))
            , Remap(remap)
            , Env(env)
            , RowIt(std::move(iterator))
        {
            Key.reserve(KeyCellDefaults->Size());

            Y_VERIFY(Key.capacity() > 0, "No key cells in part scheme");
            Y_VERIFY(Remap, "Remap cannot be NULL");
        }

        static TAutoPtr<TMemIt> Make(
                const TMemTable& memTable,
                const NMem::TTreeSnapshot& snapshot,
                TCells key,
                ESeek seek,
                TIntrusiveConstPtr<TKeyCellDefaults> keyDefaults,
                const TRemap *remap,
                IPages *env,
                EDirection direction = EDirection::Forward) noexcept
        {
            auto *iter = new TMemIt(&memTable, std::move(keyDefaults), remap, env, snapshot.Iterator());

            switch (direction) {
                case EDirection::Forward:
                    iter->Seek(key, seek);
                    break;
                case EDirection::Reverse:
                    iter->SeekReverse(key, seek);
                    break;
            }

            return iter;
        }

        void Seek(TCells key, ESeek seek) noexcept
        {
            Key.clear();
            CurrentVersion = nullptr;

            if (key) {
                NMem::TPoint search{ key, *KeyCellDefaults };

                switch (seek) {
                    case ESeek::Lower:
                        RowIt.SeekLowerBound(search);
                        break;
                    case ESeek::Exact:
                        RowIt.SeekExact(search);
                        break;
                    case ESeek::Upper:
                        RowIt.SeekUpperBound(search);
                        break;
                }
            } else {
                switch (seek) {
                    case ESeek::Lower:
                        RowIt.SeekFirst();
                        break;
                    case ESeek::Exact:
                    case ESeek::Upper:
                        RowIt.Invalidate();
                        break;
                }
            }
        }

        void SeekReverse(TCells key, ESeek seek) noexcept
        {
            Key.clear();
            CurrentVersion = nullptr;

            if (key) {
                NMem::TPoint search{ key, *KeyCellDefaults };

                switch (seek) {
                    case ESeek::Exact:
                        RowIt.SeekExact(search);
                        break;
                    case ESeek::Lower:
                        RowIt.SeekUpperBound(search, /* backwards */ true);
                        break;
                    case ESeek::Upper:
                        RowIt.SeekLowerBound(search, /* backwards */ true);
                        break;
                }
            } else {
                switch (seek) {
                    case ESeek::Lower:
                        RowIt.SeekLast();
                        break;
                    case ESeek::Exact:
                    case ESeek::Upper:
                        RowIt.Invalidate();
                        break;
                }
            }
        }

        TDbTupleRef GetKey() const
        {
            Y_VERIFY_DEBUG(IsValid());

            const ui32 len = MemTable->Scheme->Keys->Size();
            const auto *key = RowIt.GetKey();

            if (len >= KeyCellDefaults->BasicTypes().size()) {
                return { KeyCellDefaults->BasicTypes().begin(), key, len };
            } else if (!Key) {
                Key.insert(Key.end(), key, key + len);
                Key.insert(Key.end(), (**KeyCellDefaults).begin() + len, (**KeyCellDefaults).end());
            }

            return { KeyCellDefaults->BasicTypes().begin(), Key.begin(), ui32(Key.size()) };
        }

        bool IsDelta() const noexcept
        {
            auto* update = GetCurrentVersion();
            Y_VERIFY(update);

            return update->RowVersion.Step == Max<ui64>();
        }

        ui64 GetDeltaTxId() const noexcept
        {
            auto* update = GetCurrentVersion();
            Y_VERIFY(update);
            Y_VERIFY(update->RowVersion.Step == Max<ui64>());

            return update->RowVersion.TxId;
        }

        void ApplyDelta(TRowState& row) const noexcept
        {
            Y_VERIFY(row.Size() == Remap->Size(), "row state doesn't match the remap index");

            auto* update = GetCurrentVersion();
            Y_VERIFY(update);
            Y_VERIFY(update->RowVersion.Step == Max<ui64>());

            if (row.Touch(update->Rop)) {
                for (auto& up : **update) {
                    ApplyColumn(row, up);
                }
            }
        }

        bool SkipDelta() noexcept
        {
            auto* update = GetCurrentVersion();
            Y_VERIFY(update);
            Y_VERIFY(update->RowVersion.Step == Max<ui64>());

            CurrentVersion = update->Next;
            return bool(CurrentVersion);
        }

        void Apply(TRowState& row, const NTable::TTransactionMap<TRowVersion>& committedTransactions) const noexcept
        {
            Y_VERIFY(row.Size() == Remap->Size(), "row state doesn't match the remap index");

            auto* update = GetCurrentVersion();
            Y_VERIFY(update);

            for (;;) {
                const bool isDelta = update->RowVersion.Step == Max<ui64>();
                if (!isDelta || committedTransactions.Find(update->RowVersion.TxId)) {
                    if (row.Touch(update->Rop)) {
                        for (auto& up : **update) {
                            ApplyColumn(row, up);
                        }
                    }
                }
                if (!isDelta) {
                    break;
                }
                if (!(update = update->Next)) {
                    break;
                }
            }
        }

        /**
         * Returns row version at which current row state materialized
         */
        TRowVersion GetRowVersion() const noexcept
        {
            auto* update = GetCurrentVersion();
            Y_VERIFY(update);
            Y_VERIFY(update->RowVersion.Step != Max<ui64>(), "GetRowVersion cannot be called on deltas");
            return update->RowVersion;
        }

        /**
         * Skips to row at the current key as seen at row version rowVersion
         *
         * Returns false if there is no such version, e.g. current key did not
         * exist or didn't have any known updates at this rowVersion.
         */
        bool SkipToRowVersion(TRowVersion rowVersion, const NTable::TTransactionMap<TRowVersion>& committedTransactions) noexcept
        {
            Y_VERIFY_DEBUG(IsValid(), "Attempt to access an invalid row");

            auto* chain = GetCurrentVersion();
            Y_VERIFY_DEBUG(chain, "Unexpected empty chain");

            // Skip uncommitted deltas
            while (chain->RowVersion.Step == Max<ui64>() && !committedTransactions.Find(chain->RowVersion.TxId)) {
                if (!(chain = chain->Next)) {
                    CurrentVersion = nullptr;
                    return false;
                }
                CurrentVersion = chain;
            }

            // Fast path check for the current version
            if (chain->RowVersion.Step != Max<ui64>()) {
                if (chain->RowVersion <= rowVersion) {
                    return true;
                }
            } else {
                auto* commitVersion = committedTransactions.Find(chain->RowVersion.TxId);
                Y_VERIFY(commitVersion);
                if (*commitVersion <= rowVersion) {
                    return true;
                }
            }

            InvisibleRowSkips++;

            while ((chain = chain->Next)) {
                if (chain->RowVersion.Step != Max<ui64>()) {
                    if (chain->RowVersion <= rowVersion) {
                        CurrentVersion = chain;
                        return true;
                    }

                    InvisibleRowSkips++;
                } else {
                    auto* commitVersion = committedTransactions.Find(chain->RowVersion.TxId);
                    if (commitVersion && *commitVersion <= rowVersion) {
                        CurrentVersion = chain;
                        return true;
                    }
                    if (commitVersion) {
                        // Only committed deltas increment InvisibleRowSkips
                        InvisibleRowSkips++;
                    }
                }
            }

            CurrentVersion = nullptr;
            return false;
        }

        bool IsValid() const
        {
            return RowIt.IsValid();
        }

        void Next()
        {
            Y_VERIFY_DEBUG(IsValid(), "Calling Next on an exhausted iterator");

            Key.clear();
            CurrentVersion = nullptr;

            RowIt.Next();
        }

        void Prev()
        {
            Y_VERIFY_DEBUG(IsValid(), "Calling Prev on an exhausted iterator");

            Key.clear();
            CurrentVersion = nullptr;

            RowIt.Prev();
        }

    private:
        void ApplyColumn(TRowState& row, const NMem::TColumnUpdate &up) const noexcept
        {
            const auto pos = Remap->Has(up.Tag);
            auto op = TCellOp::Decode(up.Op);

            if (!pos || row.IsFinalized(pos)) {
                /* Out of remap or row slot is already filled */
            } else if (op == ELargeObj::Inline) {
                row.Set(pos, op, up.Value);
            } else if (op != ELargeObj::Extern) {
                Y_FAIL("Got an unknown ELargeObj reference type");
            } else {
                const auto ref = up.Value.AsValue<ui64>();

                if (auto blob = Env->Locate(MemTable, ref, up.Tag)) {
                    const auto got = NPage::TLabelWrapper().Read(**blob);

                    Y_VERIFY(got == NPage::ECodec::Plain && got.Version == 0);

                    row.Set(pos, { ECellOp(op), ELargeObj::Inline }, TCell(*got));
                } else {
                    op = TCellOp(blob.Need ? ECellOp::Null : ECellOp(op), ELargeObj::GlobId);

                    row.Set(pos, op, TCell::Make(MemTable->GetBlobs()->Get(ref).GId));
                }
            }
        }

        const NMem::TUpdate* GetCurrentVersion() const noexcept
        {
            Y_VERIFY_DEBUG(IsValid(), "Attempt to access an invalid row");

            if (!CurrentVersion) {
                CurrentVersion = RowIt.GetValue();
                Y_VERIFY_DEBUG(CurrentVersion, "Unexpected empty chain");
            }

            return CurrentVersion;
        }

    public:
        const TMemTable *MemTable = nullptr;
        const TIntrusiveConstPtr<TKeyCellDefaults> KeyCellDefaults;
        const TRemap* Remap = nullptr;
        IPages * const Env = nullptr;
        ui64 InvisibleRowSkips = 0;

    private:
        NMem::TTreeIterator RowIt;
        mutable TSmallVec<TCell> Key;
        mutable const NMem::TUpdate* CurrentVersion = nullptr;
    };

}
}
