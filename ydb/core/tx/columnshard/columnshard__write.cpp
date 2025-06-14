#include "columnshard_impl.h"

#include "blobs_action/transaction/tx_blobs_written.h"
#include "blobs_action/transaction/tx_draft.h"
#include "blobs_action/transaction/tx_write.h"
#include "common/limits.h"
#include "counters/columnshard.h"
#include "engines/column_engine_logs.h"
#include "operations/batch_builder/builder.h"
#include "operations/manager.h"
#include "operations/write_data.h"
#include "transactions/operators/ev_write/primary.h"
#include "transactions/operators/ev_write/secondary.h"
#include "transactions/operators/ev_write/sync.h"

#include <ydb/core/tx/columnshard/tablet/write_queue.h>
#include <ydb/core/tx/conveyor_composite/usage/service.h>
#include <ydb/core/tx/data_events/events.h>

namespace NKikimr::NColumnShard {

using namespace NTabletFlatExecutor;

void TColumnShard::OverloadWriteFail(const EOverloadStatus overloadReason, const NEvWrite::TWriteMeta& writeMeta, const ui64 writeSize,
    const ui64 cookie, std::unique_ptr<NActors::IEventBase>&& event, const TActorContext& ctx) {
    Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);
    Counters.GetCSCounters().OnWriteOverload(overloadReason, writeSize);
    switch (overloadReason) {
        case EOverloadStatus::Disk:
            Counters.OnWriteOverloadDisk();
            break;
        case EOverloadStatus::InsertTable:
            Counters.OnWriteOverloadInsertTable(writeSize);
            break;
        case EOverloadStatus::OverloadMetadata:
            Counters.OnWriteOverloadMetadata(writeSize);
            break;
        case EOverloadStatus::OverloadCompaction:
            Counters.OnWriteOverloadCompaction(writeSize);
            break;
        case EOverloadStatus::ShardTxInFly:
            Counters.OnWriteOverloadShardTx(writeSize);
            break;
        case EOverloadStatus::ShardWritesInFly:
            Counters.OnWriteOverloadShardWrites(writeSize);
            break;
        case EOverloadStatus::ShardWritesSizeInFly:
            Counters.OnWriteOverloadShardWritesSize(writeSize);
            break;
        case EOverloadStatus::None:
            Y_ABORT("invalid function usage");
    }

    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "write_overload")("size", writeSize)("path_id", writeMeta.GetTableId())(
        "reason", overloadReason);

    ctx.Send(writeMeta.GetSource(), event.release(), 0, cookie);
}

TColumnShard::EOverloadStatus TColumnShard::CheckOverloadedWait(const TInternalPathId pathId) const {
    if (InsertTable && InsertTable->IsOverloadedByCommitted(pathId)) {
        return EOverloadStatus::InsertTable;
    }
    Counters.GetCSCounters().OnIndexMetadataLimit(NOlap::IColumnEngine::GetMetadataLimit());
    if (TablesManager.GetPrimaryIndex()) {
        if (TablesManager.GetPrimaryIndex()->IsOverloadedByMetadata(NOlap::IColumnEngine::GetMetadataLimit())) {
            return EOverloadStatus::OverloadMetadata;
        }
        if (TablesManager.GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>()
                .GetGranuleVerified(pathId)
                .GetOptimizerPlanner()
                .IsOverloaded()) {
            return EOverloadStatus::OverloadCompaction;
        }
    }
    return EOverloadStatus::None;
}

TColumnShard::EOverloadStatus TColumnShard::CheckOverloadedImmediate(const TInternalPathId /* pathId */) const {
    if (IsAnyChannelYellowStop()) {
        return EOverloadStatus::Disk;
    }
    ui64 txLimit = Settings.OverloadTxInFlight;
    const ui64 writesLimit = HasAppData() ? AppDataVerified().ColumnShardConfig.GetWritingInFlightRequestsCountLimit() : 1000000;
    const ui64 writesSizeLimit = HasAppData() ? AppDataVerified().ColumnShardConfig.GetWritingInFlightRequestBytesLimit() : (((ui64)1) << 30);
    if (txLimit && Executor()->GetStats().TxInFly > txLimit) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "shard_overload")("reason", "tx_in_fly")("sum", Executor()->GetStats().TxInFly)(
            "limit", txLimit);
        return EOverloadStatus::ShardTxInFly;
    }
    if (writesLimit && Counters.GetWritesMonitor()->GetWritesInFlight() > writesLimit) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "shard_overload")("reason", "writes_in_fly")(
            "sum", Counters.GetWritesMonitor()->GetWritesInFlight())("limit", writesLimit);
        return EOverloadStatus::ShardWritesInFly;
    }
    if (writesSizeLimit && Counters.GetWritesMonitor()->GetWritesSizeInFlight() > writesSizeLimit) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "shard_overload")("reason", "writes_size_in_fly")(
            "sum", Counters.GetWritesMonitor()->GetWritesSizeInFlight())("limit", writesSizeLimit);
        return EOverloadStatus::ShardWritesSizeInFly;
    }
    return EOverloadStatus::None;
}

void TColumnShard::Handle(NPrivateEvents::NWrite::TEvWritePortionResult::TPtr& ev, const TActorContext& ctx) {
    TMemoryProfileGuard mpg("TEvWritePortionResult");
    NActors::TLogContextGuard gLogging =
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_WRITE)("tablet_id", TabletID())("event", "TEvWritePortionResult");
    TInsertedPortions writtenData = ev->Get()->DetachInsertedData();
    if (ev->Get()->GetWriteStatus() == NKikimrProto::OK) {
        const TMonotonic now = TMonotonic::Now();
        for (auto&& i : writtenData.GetWriteResults()) {
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_WRITE)("writing_size", i.GetDataSize())("event", "data_write_finished")(
                "writing_id", i.GetWriteMeta().GetId());
            i.MutableWriteMeta().OnStage(NEvWrite::EWriteStage::Finished);
            Counters.OnWritePutBlobsSuccess(now - i.GetWriteMeta().GetWriteStartInstant(), i.GetRecordsCount());
            Counters.GetWritesMonitor()->OnFinishWrite(i.GetDataSize(), 1);
        }
        Execute(new TTxBlobsWritingFinished(this, ev->Get()->GetWriteStatus(), ev->Get()->GetWriteAction(), std::move(writtenData)), ctx);
    } else {
        const TMonotonic now = TMonotonic::Now();
        for (auto&& i : writtenData.GetWriteResults()) {
            Counters.OnWritePutBlobsFailed(now - i.GetWriteMeta().GetWriteStartInstant(), i.GetRecordsCount());
            Counters.GetCSCounters().OnWritePutBlobsFail(now - i.GetWriteMeta().GetWriteStartInstant());
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("writing_size", i.GetDataSize())("event", "data_write_error")(
                "writing_id", i.GetWriteMeta().GetId())("reason", i.GetErrorMessage());
            Counters.GetWritesMonitor()->OnFinishWrite(i.GetDataSize(), 1);
            i.MutableWriteMeta().OnStage(NEvWrite::EWriteStage::Finished);
        }

        Execute(new TTxBlobsWritingFailed(this, std::move(writtenData)), ctx);
    }
}

void TColumnShard::Handle(TEvPrivate::TEvWriteBlobsResult::TPtr& ev, const TActorContext& ctx) {
    NActors::TLogContextGuard gLogging =
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_WRITE)("tablet_id", TabletID())("event", "TEvWriteBlobsResult");

    auto& putResult = ev->Get()->GetPutResult();
    OnYellowChannels(putResult);
    NOlap::TWritingBuffer& wBuffer = ev->Get()->MutableWritesBuffer();
    auto baseAggregations = wBuffer.GetAggregations();
    wBuffer.InitReplyReceived(TMonotonic::Now());

    for (auto&& aggr : baseAggregations) {
        const auto& writeMeta = aggr->GetWriteMeta();
        aggr->MutableWriteMeta().OnStage(NEvWrite::EWriteStage::Finished);
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "blobs_write_finished")("writing_size", aggr->GetSize())(
            "writing_id", writeMeta.GetId())("status", putResult.GetPutStatus());
        Counters.GetWritesMonitor()->OnFinishWrite(aggr->GetSize(), 1);

        if (putResult.GetPutStatus() != NKikimrProto::OK) {
            Counters.GetCSCounters().OnWritePutBlobsFail(TMonotonic::Now() - writeMeta.GetWriteStartInstant());
            Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);

            auto errCode = NKikimrTxColumnShard::EResultStatus::STORAGE_ERROR;
            if (putResult.GetPutStatus() == NKikimrProto::TIMEOUT || putResult.GetPutStatus() == NKikimrProto::DEADLINE) {
                errCode = NKikimrTxColumnShard::EResultStatus::TIMEOUT;
            } else if (putResult.GetPutStatus() == NKikimrProto::TRYLATER || putResult.GetPutStatus() == NKikimrProto::OUT_OF_SPACE) {
                errCode = NKikimrTxColumnShard::EResultStatus::OVERLOADED;
            } else if (putResult.GetPutStatus() == NKikimrProto::CORRUPTED) {
                errCode = NKikimrTxColumnShard::EResultStatus::ERROR;
            }

            if (writeMeta.HasLongTxId()) {
                auto result = std::make_unique<TEvColumnShard::TEvWriteResult>(TabletID(), writeMeta, errCode);
                ctx.Send(writeMeta.GetSource(), result.release());
            } else {
                auto operation = OperationsManager->GetOperationVerified((TOperationWriteId)writeMeta.GetWriteId());
                auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(TabletID(), operation->GetLockId(),
                    ev->Get()->GetWriteResultStatus(), ev->Get()->GetErrorMessage() ? ev->Get()->GetErrorMessage() : "put data fails");
                ctx.Send(writeMeta.GetSource(), result.release(), 0, operation->GetCookie());
            }
            Counters.GetCSCounters().OnFailedWriteResponse(EWriteFailReason::PutBlob);
            wBuffer.RemoveData(aggr, StoragesManager->GetInsertOperator());
        } else {
            const TMonotonic now = TMonotonic::Now();
            Counters.OnWritePutBlobsSuccess(now - writeMeta.GetWriteStartInstant(), aggr->GetRows());
            LOG_S_DEBUG("Write (record) into pathId " << writeMeta.GetTableId()
                                                      << (writeMeta.GetWriteId() ? (" writeId " + ToString(writeMeta.GetWriteId())).c_str() : "")
                                                      << " at tablet " << TabletID());
        }
    }
    Execute(new TTxWrite(this, ev), ctx);
}

void TColumnShard::Handle(TEvPrivate::TEvWriteDraft::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxWriteDraft(this, ev->Get()->WriteController), ctx);
}

void TColumnShard::Handle(TEvColumnShard::TEvWrite::TPtr& ev, const TActorContext& ctx) {
    Counters.GetCSCounters().OnStartWriteRequest();

    const auto& record = Proto(ev->Get());
    const auto pathId = TInternalPathId::FromRawValue(record.GetTableId());
    const ui64 writeId = record.GetWriteId();
    const ui64 cookie = ev->Cookie;
    const TString dedupId = record.GetDedupId();
    const auto source = ev->Sender;

    Counters.GetColumnTablesCounters()->GetPathIdCounter(pathId)->OnWriteEvent();

    std::optional<ui32> granuleShardingVersion;
    if (record.HasGranuleShardingVersion()) {
        granuleShardingVersion = record.GetGranuleShardingVersion();
    }

    auto writeMetaPtr = std::make_shared<NEvWrite::TWriteMeta>(writeId, pathId, source, granuleShardingVersion,
        TGUID::CreateTimebased().AsGuidString(), Counters.GetCSCounters().WritingCounters->GetWriteFlowCounters());
    auto& writeMeta = *writeMetaPtr;
    if (record.HasModificationType()) {
        writeMeta.SetModificationType(TEnumOperator<NEvWrite::EModificationType>::DeserializeFromProto(record.GetModificationType()));
    }
    writeMeta.SetDedupId(dedupId);
    Y_ABORT_UNLESS(record.HasLongTxId());
    writeMeta.SetLongTxId(NLongTxService::TLongTxId::FromProto(record.GetLongTxId()));
    writeMeta.SetWritePartId(record.GetWritePartId());

    const auto returnFail = [&](const NColumnShard::ECumulativeCounters signalIndex, const EWriteFailReason reason,
                                NKikimrTxColumnShard::EResultStatus resultStatus) {
        Counters.GetTabletCounters()->IncCounter(signalIndex);

        ctx.Send(source, std::make_unique<TEvColumnShard::TEvWriteResult>(TabletID(), writeMeta, resultStatus));
        Counters.GetCSCounters().OnFailedWriteResponse(reason);
        return;
    };

    if (SpaceWatcher->SubDomainOutOfSpace &&
        (!record.HasModificationType() || (record.GetModificationType() != NKikimrTxColumnShard::TEvWrite::OPERATION_DELETE))) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "skip_writing")("reason", "quota_exceeded");
        Counters.GetTabletCounters()->IncCounter(COUNTER_OUT_OF_SPACE);
        return returnFail(COUNTER_WRITE_FAIL, EWriteFailReason::Overload, NKikimrTxColumnShard::EResultStatus::OVERLOADED);
    }

    if (!AppDataVerified().ColumnShardConfig.GetWritingEnabled()) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "skip_writing")("reason", "disabled");
        return returnFail(COUNTER_WRITE_FAIL, EWriteFailReason::Disabled, NKikimrTxColumnShard::EResultStatus::ERROR);
    }

    if (!TablesManager.IsReadyForStartWrite(pathId, false)) {
        LOG_S_NOTICE("Write (fail) into pathId:" << writeMeta.GetTableId() << (TablesManager.HasPrimaryIndex() ? "" : " no index")
                                                 << " at tablet " << TabletID());

        return returnFail(COUNTER_WRITE_FAIL, EWriteFailReason::NoTable, NKikimrTxColumnShard::EResultStatus::ERROR);
    }

    {
        auto status = TablesManager.GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>()
                          .GetGranuleVerified(writeMeta.GetTableId())
                          .GetOptimizerPlanner()
                          .CheckWriteData();
        if (status.IsFail()) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "writing_fail_through_compaction")("reason", status.GetErrorMessage());
            return returnFail(COUNTER_WRITE_FAIL, EWriteFailReason::CompactionCriteria, NKikimrTxColumnShard::EResultStatus::ERROR);
        }
    }

    const auto& snapshotSchema = TablesManager.GetPrimaryIndex()->GetVersionedIndex().GetLastSchema();
    auto arrowData = std::make_shared<TProtoArrowData>(snapshotSchema);
    if (!arrowData->ParseFromProto(record)) {
        LOG_S_ERROR(
            "Write (fail) " << record.GetData().size() << " bytes into pathId " << writeMeta.GetTableId() << " at tablet " << TabletID());
        return returnFail(COUNTER_WRITE_FAIL, EWriteFailReason::IncorrectSchema, NKikimrTxColumnShard::EResultStatus::ERROR);
    }

    NEvWrite::TWriteData writeData(writeMetaPtr, arrowData, snapshotSchema->GetIndexInfo().GetReplaceKey(),
        StoragesManager->GetInsertOperator()->StartWritingAction(NOlap::NBlobOperations::EConsumer::WRITING), false);
    auto overloadStatus = CheckOverloadedImmediate(pathId);
    if (overloadStatus == EOverloadStatus::None) {
        overloadStatus = CheckOverloadedWait(pathId);
    }
    if (overloadStatus != EOverloadStatus::None) {
        std::unique_ptr<NActors::IEventBase> result = std::make_unique<TEvColumnShard::TEvWriteResult>(
            TabletID(), writeData.GetWriteMeta(), NKikimrTxColumnShard::EResultStatus::OVERLOADED);
        OverloadWriteFail(overloadStatus, writeData.GetWriteMeta(), writeData.GetSize(), cookie, std::move(result), ctx);
        Counters.GetCSCounters().OnFailedWriteResponse(EWriteFailReason::Overload);
    } else {
        if (ui64 writeId = (ui64)HasLongTxWrite(writeMeta.GetLongTxIdUnsafe(), writeMeta.GetWritePartId())) {
            LOG_S_DEBUG("Write (duplicate) into pathId " << writeMeta.GetTableId() << " longTx " << writeMeta.GetLongTxIdUnsafe().ToString()
                                                         << " at tablet " << TabletID());

            Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_DUPLICATE);

            auto result =
                std::make_unique<TEvColumnShard::TEvWriteResult>(TabletID(), writeMeta, writeId, NKikimrTxColumnShard::EResultStatus::SUCCESS);
            ctx.Send(writeMeta.GetSource(), result.release());
            Counters.GetCSCounters().OnFailedWriteResponse(EWriteFailReason::LongTxDuplication);
            return;
        }

        Counters.GetWritesMonitor()->OnStartWrite(writeData.GetSize());

        LOG_S_DEBUG("Write (blob) " << writeData.GetSize() << " bytes into pathId " << writeMeta.GetTableId()
                                    << (writeMeta.GetWriteId() ? (" writeId " + ToString(writeMeta.GetWriteId())).c_str() : " ")
                                    << Counters.GetWritesMonitor()->DebugString() << " at tablet " << TabletID());

        NOlap::TWritingContext context(TabletID(), SelfId(), snapshotSchema, StoragesManager, Counters.GetIndexationCounters().SplitterCounters,
            Counters.GetCSCounters().WritingCounters, GetLastTxSnapshot(), std::make_shared<TAtomicCounter>(1), true,
            BufferizationInsertionWriteActorId, BufferizationPortionsWriteActorId);
        std::shared_ptr<NConveyor::ITask> task = std::make_shared<NOlap::TBuildBatchesTask>(std::move(writeData), context);
        NConveyorComposite::TInsertServiceOperator::SendTaskToExecute(task);
    }
}

class TCommitOperation {
private:
    const ui64 TabletId;

public:
    using TPtr = std::shared_ptr<TCommitOperation>;

    bool NeedSyncLocks() const {
        return SendingShards.size() || ReceivingShards.size();
    }

    bool IsPrimary() const {
        AFL_VERIFY(NeedSyncLocks());
        return TabletId == ArbiterColumnShard;
    }

    TCommitOperation(const ui64 tabletId)
        : TabletId(tabletId) {
    }

    TConclusionStatus Parse(const NEvents::TDataEvents::TEvWrite& evWrite) {
        TxId = evWrite.Record.GetTxId();
        NActors::TLogContextGuard lGuard = NActors::TLogContextBuilder::Build()("tx_id", TxId);
        const auto& locks = evWrite.Record.GetLocks();
        AFL_VERIFY(!locks.GetLocks().empty());
        auto& lock = locks.GetLocks()[0];
        LockId = lock.GetLockId();
        SendingShards = std::set<ui64>(locks.GetSendingShards().begin(), locks.GetSendingShards().end());
        ReceivingShards = std::set<ui64>(locks.GetReceivingShards().begin(), locks.GetReceivingShards().end());
        if (SendingShards.empty() != ReceivingShards.empty()) {
            return TConclusionStatus::Fail("incorrect synchronization data (send/receiving lists)");
        }
        if (ReceivingShards.size() && SendingShards.size()) {
            if (!ReceivingShards.contains(TabletId) && !SendingShards.contains(TabletId)) {
                return TConclusionStatus::Fail("current tablet_id is absent in sending and receiving lists");
            }
            if (!locks.HasArbiterColumnShard()) {
                return TConclusionStatus::Fail("no arbiter info in request");
            }
            ArbiterColumnShard = locks.GetArbiterColumnShard();

            if (IsPrimary() && !ReceivingShards.contains(ArbiterColumnShard)) {
                AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "incorrect arbiter")("arbiter_id", ArbiterColumnShard)(
                    "receiving", JoinSeq(", ", ReceivingShards))("sending", JoinSeq(", ", SendingShards));
                return TConclusionStatus::Fail("arbiter is absent in receiving lists");
            }
            if (!IsPrimary() && (!ReceivingShards.contains(ArbiterColumnShard) || !SendingShards.contains(ArbiterColumnShard))) {
                AFL_WARN(NKikimrServices::TX_COLUMNSHARD_WRITE)("event", "incorrect arbiter")("arbiter_id", ArbiterColumnShard)(
                    "receiving", JoinSeq(", ", ReceivingShards))("sending", JoinSeq(", ", SendingShards));
                return TConclusionStatus::Fail("arbiter is absent in sending or receiving lists");
            }
        }

        Generation = lock.GetGeneration();
        InternalGenerationCounter = lock.GetCounter();
        if (!GetLockId()) {
            return TConclusionStatus::Fail("not initialized lock info in commit message");
        }
        if (!TxId) {
            return TConclusionStatus::Fail("not initialized TxId for commit event");
        }
        if (locks.GetOp() != NKikimrDataEvents::TKqpLocks::Commit) {
            return TConclusionStatus::Fail("incorrect message type");
        }
        return TConclusionStatus::Success();
    }

    std::unique_ptr<NColumnShard::TEvWriteCommitSyncTransactionOperator> CreateTxOperator(
        const NKikimrTxColumnShard::ETransactionKind kind) const {
        if (IsPrimary()) {
            return std::make_unique<NColumnShard::TEvWriteCommitPrimaryTransactionOperator>(
                TFullTxInfo::BuildFake(kind), LockId, ReceivingShards, SendingShards);
        } else {
            return std::make_unique<NColumnShard::TEvWriteCommitSecondaryTransactionOperator>(
                TFullTxInfo::BuildFake(kind), LockId, ArbiterColumnShard, ReceivingShards.contains(TabletId));
        }
    }

private:
    YDB_READONLY(ui64, LockId, 0);
    YDB_READONLY(ui64, Generation, 0);
    YDB_READONLY(ui64, InternalGenerationCounter, 0);
    YDB_READONLY(ui64, TxId, 0);
    YDB_READONLY_DEF(std::set<ui64>, SendingShards);
    YDB_READONLY_DEF(std::set<ui64>, ReceivingShards);
    ui64 ArbiterColumnShard = 0;
};

class TProposeWriteTransaction: public TExtendedTransactionBase {
private:
    using TBase = TExtendedTransactionBase;

public:
    TProposeWriteTransaction(TColumnShard* self, TCommitOperation::TPtr op, const TActorId source, const ui64 cookie)
        : TBase(self, "TProposeWriteTransaction")
        , WriteCommit(op)
        , Source(source)
        , Cookie(cookie) {
    }

    virtual bool DoExecute(TTransactionContext& txc, const TActorContext&) override {
        NKikimrTxColumnShard::TCommitWriteTxBody proto;
        NKikimrTxColumnShard::ETransactionKind kind;
        if (WriteCommit->NeedSyncLocks()) {
            if (WriteCommit->IsPrimary()) {
                kind = NKikimrTxColumnShard::TX_KIND_COMMIT_WRITE_PRIMARY;
            } else {
                kind = NKikimrTxColumnShard::TX_KIND_COMMIT_WRITE_SECONDARY;
            }
            proto = WriteCommit->CreateTxOperator(kind)->SerializeToProto();
        } else {
            kind = NKikimrTxColumnShard::TX_KIND_COMMIT_WRITE;
        }
        proto.SetLockId(WriteCommit->GetLockId());
        TxOperator = Self->GetProgressTxController().StartProposeOnExecute(
            TTxController::TTxInfo(kind, WriteCommit->GetTxId(), Source, Self->GetProgressTxController().GetAllowedStep(), 
            Cookie, {}), proto.SerializeAsString(), txc);
        return true;
    }

    virtual void DoComplete(const TActorContext& ctx) override {
        Self->GetProgressTxController().FinishProposeOnComplete(WriteCommit->GetTxId(), ctx);
    }
    TTxType GetTxType() const override {
        return TXTYPE_PROPOSE;
    }

private:
    TCommitOperation::TPtr WriteCommit;
    TActorId Source;
    ui64 Cookie;
    std::shared_ptr<TTxController::ITransactionOperator> TxOperator;
};

class TAbortWriteTransaction: public NTabletFlatExecutor::TTransactionBase<TColumnShard> {
private:
    using TBase = NTabletFlatExecutor::TTransactionBase<TColumnShard>;

public:
    TAbortWriteTransaction(TColumnShard* self, const ui64 txId, const TActorId source, const ui64 cookie)
        : TBase(self)
        , TxId(txId)
        , Source(source)
        , Cookie(cookie) {
    }

    virtual bool Execute(TTransactionContext& txc, const TActorContext&) override {
        Self->GetOperationsManager().AbortTransactionOnExecute(*Self, TxId, txc);
        return true;
    }

    virtual void Complete(const TActorContext& ctx) override {
        Self->GetOperationsManager().AbortTransactionOnComplete(*Self, TxId);
        auto result = NEvents::TDataEvents::TEvWriteResult::BuildCompleted(Self->TabletID(), TxId);
        ctx.Send(Source, result.release(), 0, Cookie);
    }
    TTxType GetTxType() const override {
        return TXTYPE_PROPOSE;
    }

private:
    ui64 TxId;
    TActorId Source;
    ui64 Cookie;
};

void TColumnShard::Handle(NEvents::TDataEvents::TEvWrite::TPtr& ev, const TActorContext& ctx) {
    TMemoryProfileGuard mpg("NEvents::TDataEvents::TEvWrite");
    NActors::TLogContextGuard gLogging =
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_WRITE)("tablet_id", TabletID())("event", "TEvWrite");

    const auto& record = ev->Get()->Record;
    const auto source = ev->Sender;
    const auto cookie = ev->Cookie;

    if (!TablesManager.GetPrimaryIndex()) {
        Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);
        auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(
            TabletID(), 0, NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST, "schema not ready for writing");
        ctx.Send(source, result.release(), 0, cookie);
        return;
    }

    const auto behaviourConclusion = TOperationsManager::GetBehaviour(*ev->Get());
    AFL_TRACE(NKikimrServices::TX_COLUMNSHARD_WRITE)("ev_write", record.DebugString());
    if (behaviourConclusion.IsFail()) {
        Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);
        auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(TabletID(), 0, NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST,
            "invalid write event: " + behaviourConclusion.GetErrorMessage());
        ctx.Send(source, result.release(), 0, cookie);
        return;
    }
    auto behaviour = *behaviourConclusion;

    if (behaviour == EOperationBehaviour::AbortWriteLock) {
        Execute(new TAbortWriteTransaction(this, record.GetLocks().GetLocks()[0].GetLockId(), source, cookie), ctx);
        return;
    }

    const auto sendError = [&](const TString& message, const NKikimrDataEvents::TEvWriteResult::EStatus status) {
        Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);
        auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(TabletID(), record.GetTxId(), status, message);
        ctx.Send(source, result.release(), 0, cookie);
    };
    if (behaviour == EOperationBehaviour::CommitWriteLock) {
        auto commitOperation = std::make_shared<TCommitOperation>(TabletID());
        auto conclusionParse = commitOperation->Parse(*ev->Get());
        if (conclusionParse.IsFail()) {
            sendError(conclusionParse.GetErrorMessage(), NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
        } else {
            auto* lockInfo = OperationsManager->GetLockOptional(commitOperation->GetLockId());
            if (!lockInfo) {
                sendError("haven't lock for commit: " + ::ToString(commitOperation->GetLockId()),
                    NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
            } else {
                if (commitOperation->NeedSyncLocks()) {
                    if (lockInfo->GetGeneration() != commitOperation->GetGeneration()) {
                        sendError("tablet lock have another generation: " + ::ToString(lockInfo->GetGeneration()) +
                                      " != " + ::ToString(commitOperation->GetGeneration()),
                            NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN);
                    } else if (lockInfo->GetInternalGenerationCounter() != commitOperation->GetInternalGenerationCounter()) {
                        sendError(
                            "tablet lock have another internal generation counter: " + ::ToString(lockInfo->GetInternalGenerationCounter()) +
                                " != " + ::ToString(commitOperation->GetInternalGenerationCounter()),
                            NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN);
                    } else {
                        Execute(new TProposeWriteTransaction(this, commitOperation, source, cookie), ctx);
                    }
                } else {
                    Execute(new TProposeWriteTransaction(this, commitOperation, source, cookie), ctx);
                }
            }
        }
        return;
    }

    if (record.GetOperations().size() != 1) {
        Counters.GetTabletCounters()->IncCounter(COUNTER_WRITE_FAIL);
        auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(
            TabletID(), 0, NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST, "only single operation is supported");
        ctx.Send(source, result.release(), 0, cookie);
        return;
    }

    const auto& operation = record.GetOperations()[0];
    const std::optional<NEvWrite::EModificationType> mType =
        TEnumOperator<NEvWrite::EModificationType>::DeserializeFromProto(operation.GetType());
    if (!mType) {
        sendError("operation " + NKikimrDataEvents::TEvWrite::TOperation::EOperationType_Name(operation.GetType()) + " is not supported",
            NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
        return;
    }

    if (!operation.GetTableId().HasSchemaVersion()) {
        sendError("schema version not set", NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
        return;
    }

    auto schema = TablesManager.GetPrimaryIndex()->GetVersionedIndex().GetSchemaOptional(operation.GetTableId().GetSchemaVersion());
    if (!schema) {
        sendError("unknown schema version", NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
        return;
    }

    const auto pathId = TInternalPathId::FromRawValue(operation.GetTableId().GetTableId());

    if (!TablesManager.IsReadyForStartWrite(pathId, false)) {
        sendError("table not writable", NKikimrDataEvents::TEvWriteResult::STATUS_INTERNAL_ERROR);
        return;
    }

    Counters.GetColumnTablesCounters()->GetPathIdCounter(pathId)->OnWriteEvent();

    auto arrowData = std::make_shared<TArrowData>(schema);
    if (!arrowData->Parse(operation, NEvWrite::TPayloadReader<NEvents::TDataEvents::TEvWrite>(*ev->Get()))) {
        sendError("parsing data error", NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST);
        return;
    }

    if (!AppDataVerified().ColumnShardConfig.GetWritingEnabled()) {
        sendError("writing disabled", NKikimrDataEvents::TEvWriteResult::STATUS_CANCELLED);
        return;
    }

    const bool outOfSpace = SpaceWatcher->SubDomainOutOfSpace && (*mType != NEvWrite::EModificationType::Delete);
    if (outOfSpace) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "skip_writing")("reason", "quota_exceeded")("source", "dataevent");
    }
    auto overloadStatus = outOfSpace ? EOverloadStatus::Disk : CheckOverloadedImmediate(pathId);
    if (overloadStatus != EOverloadStatus::None) {
        std::unique_ptr<NActors::IEventBase> result = NEvents::TDataEvents::TEvWriteResult::BuildError(
            TabletID(), 0, NKikimrDataEvents::TEvWriteResult::STATUS_OVERLOADED, "overload data error");
        OverloadWriteFail(overloadStatus,
            NEvWrite::TWriteMeta(0, pathId, source, {}, TGUID::CreateTimebased().AsGuidString(),
                Counters.GetCSCounters().WritingCounters->GetWriteFlowCounters()),
            arrowData->GetSize(), cookie, std::move(result), ctx);
        return;
    }

    std::optional<ui32> granuleShardingVersionId;
    if (record.HasGranuleShardingVersionId()) {
        granuleShardingVersionId = record.GetGranuleShardingVersionId();
    }

    ui64 lockId = 0;
    if (behaviour == EOperationBehaviour::NoTxWrite) {
        lockId = BuildEphemeralTxId();
    } else {
        lockId = record.GetLockTxId();
    }

    Counters.GetWritesMonitor()->OnStartWrite(arrowData->GetSize());
    WriteTasksQueue->Enqueue(TWriteTask(arrowData, schema, source, granuleShardingVersionId, pathId, cookie, lockId, *mType, behaviour));
    WriteTasksQueue->Drain(false, ctx);
}

}   // namespace NKikimr::NColumnShard
