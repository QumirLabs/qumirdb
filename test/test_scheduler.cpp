#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>

namespace {

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

struct TRowSet {
    std::vector<std::string> Col;
    std::vector<int> Selection;

    bool Empty() const {
        return Col.empty();
    }
};

void GenerateKey(std::string& key, int* seed) {
    int len = 2 + RandomInt(seed) % 5;
    key.resize(len);
    for (int i = 0; i < len; i++) {
        key[i] = 'a' + RandomInt(seed) % ('z' - 'a' + 1);
    }
}

void GenerateRowSet(TRowSet& rowSet, int size, int* seed) {
    rowSet.Col.clear();
    rowSet.Selection.clear();
    std::string key;
    for (int i = 0; i < size; i++) {
        GenerateKey(key, seed);
        rowSet.Col.push_back(key);
        rowSet.Selection.push_back(1);
    }
}

template <class T>
class TSPSC {
public:
    explicit TSPSC(size_t capacity)
        : Capacity_(capacity + 1)
        , Buffer_(Capacity_)
    {
        assert(capacity > 0);
    }

    bool CanPush() const {
        auto tail = Tail_.load(std::memory_order_relaxed);
        auto next = Next(tail);
        auto head = Head_.load(std::memory_order_acquire);
        return next != head;
    }

    bool TryPush(T&& value) {
        auto tail = Tail_.load(std::memory_order_relaxed);
        auto next = Next(tail);
        if (next == Head_.load(std::memory_order_acquire)) {
            return false;
        }

        Buffer_[tail].emplace(std::move(value));
        Tail_.store(next, std::memory_order_release);
        return true;
    }

    bool TryPop(T& value) {
        auto head = Head_.load(std::memory_order_relaxed);
        if (head == Tail_.load(std::memory_order_acquire)) {
            return false;
        }

        value = std::move(*Buffer_[head]);
        Buffer_[head].reset();
        Head_.store(Next(head), std::memory_order_release);
        return true;
    }

private:
    size_t Next(size_t index) const {
        ++index;
        return index == Capacity_ ? 0 : index;
    }

    size_t Capacity_;
    std::vector<std::optional<T>> Buffer_;
    std::atomic<size_t> Head_ = 0;
    std::atomic<size_t> Tail_ = 0;
};

enum class EConnectionKind {
    OneToOne,    // i -> i
    Gather,      // many -> one, unordered
    Merge,       // many -> one, sorted
    HashShuffle, // many -> many split by hash(keys)
    Broadcast,   // one/many -> all
    RoundRobin,  // many -> many in random order
};

enum class EState {
    OK = 0,
    NEED_DATA = 1,
    BLOCKED_OUTPUT = 2,
    FINISHED = 3,
};

enum class EFetchState {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

enum class ETaskState {
    Idle = 0,
    Queued = 1,
    Running = 2,
    Reschedule = 3,
    Finished = 4,
};

struct IConnection {
    virtual ~IConnection() = default;

    virtual EConnectionKind Kind() const = 0;
    virtual void Resize(size_t srcSize, size_t dstSize) = 0;

    // output side
    // src_id - номер партиции группы входных тасок
    virtual bool CanPush(int src_id) const = 0;
    virtual void Push(int src_id, TRowSet&& rowSet) = 0;
    virtual void Finish(int src_id) = 0;

    // input side
    // dst_id - номер партиции группы выходных тасок
    virtual EFetchState Fetch(int dst_id, TRowSet& batch) = 0;
};

struct TInputPort {
    IConnection* Conn = nullptr;
    int DstId = 0;

    EFetchState Fetch(TRowSet& batch) {
        return Conn->Fetch(DstId, batch);
    }
};

struct TOutputPort {
    IConnection* Conn = nullptr;
    int SrcId = 0;

    bool CanPush() const {
        if (!Conn) {
            return true;
        }
        return Conn->CanPush(SrcId);
    }

    void Push(TRowSet&& rowSet) {
        if (Conn) {
            Conn->Push(SrcId, std::move(rowSet));
        }
    }

    void Finish() {
        if (Conn) {
            Conn->Finish(SrcId);
        }
    }
};

static constexpr int MaxOutput = 1;

struct TOneToOne : IConnection {
    TOneToOne() {
        Resize(1, 1);
    }

    EConnectionKind Kind() const override {
        return EConnectionKind::OneToOne;
    }

    void Resize(size_t srcSize, size_t dstSize) override {
        assert(srcSize == dstSize);
        Size_ = srcSize;
        Finished_ = std::vector<std::atomic<bool>>(Size_);
        Outputs_.clear();
        Outputs_.reserve(Size_);
        for (size_t i = 0; i < Size_; ++i) {
            Outputs_.push_back(std::make_unique<TSPSC<TRowSet>>(MaxOutput));
        }
    }

    // Output
    bool CanPush(int src_id) const override {
        assert(src_id >= 0 && static_cast<size_t>(src_id) < Size_);
        return Outputs_[src_id]->CanPush();
    }

    void Push(int src_id, TRowSet&& rowSet) override {
        assert(src_id >= 0 && static_cast<size_t>(src_id) < Size_);
        auto pushed = Outputs_[src_id]->TryPush(std::move(rowSet));
        assert(pushed);
    }

    void Finish(int src_id) override {
        assert(src_id >= 0 && static_cast<size_t>(src_id) < Size_);
        Finished_[src_id].store(true, std::memory_order_release);
    }

    // Input
    EFetchState Fetch(int dst_id, TRowSet& rowSet) override {
        assert(dst_id >= 0 && static_cast<size_t>(dst_id) < Size_);
        if (Outputs_[dst_id]->TryPop(rowSet)) {
            return EFetchState::OK;
        }
        if (!Finished_[dst_id].load(std::memory_order_acquire)) {
            return EFetchState::NO_DATA;
        }
        // The last batch may have been pushed after the failed pop above.
        return Outputs_[dst_id]->TryPop(rowSet) ? EFetchState::OK : EFetchState::FINISHED;
    }

    size_t Size_ = 0;
    std::vector<std::atomic<bool>> Finished_;
    std::vector<std::unique_ptr<TSPSC<TRowSet>>> Outputs_;
};

// many -> one
struct TGather : IConnection {
    TGather(size_t partitions = 1)
    {
        Resize(partitions, 1);
    }

    EConnectionKind Kind() const override {
        return EConnectionKind::Gather;
    }

    void Resize(size_t srcSize, size_t dstSize) override {
        assert(dstSize == 1);
        Size_ = srcSize;
        FetchId_ = 0;
        NFinished_ = 0;
        Flags_.assign(Size_, 0);
        Outputs_.clear();
        Outputs_.reserve(Size_);
        for (size_t i = 0; i < Size_; ++i) {
            Outputs_.push_back(std::make_unique<TSPSC<TRowSet>>(MaxOutput));
        }
    }

    bool CanPush(int src_id) const override {
        return Outputs_[src_id]->CanPush();
    }

    void Finish(int src_id) override {
        // Sources finish on different threads; only the counter is shared.
        if (Flags_[src_id] == 0) {
            Flags_[src_id] = 1;
            NFinished_.fetch_add(1, std::memory_order_release);
        }
    }

    void Push(int src_id, TRowSet&& rowSet) override {
        auto pushed = Outputs_[src_id]->TryPush(std::move(rowSet));
        assert(pushed);
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) override {
        assert(dst_id == 0);

        auto fetch = [&]() {
            auto from = (FetchId_ + 1) % Size_;
            FetchId_ = from;
            for (size_t i = 0; i < Size_; ++i) {
                auto index = (from + i) % Size_;
                if (Outputs_[index]->TryPop(batch)) {
                    return true;
                }
            }
            return false;
        };

        if (fetch()) {
            return EFetchState::OK;
        }
        if (NFinished_.load(std::memory_order_acquire) != Size_) {
            return EFetchState::NO_DATA;
        }
        // The last batches may have been pushed after the failed fetch above.
        return fetch() ? EFetchState::OK : EFetchState::FINISHED;
    }

    size_t Size_;
    size_t FetchId_ = 0;
    std::atomic<size_t> NFinished_ = 0;
    std::vector<int> Flags_;
    std::vector<std::unique_ptr<TSPSC<TRowSet>>> Outputs_;
};

// many -> one
// assume records are sorted
struct TMerge : IConnection {
    EConnectionKind Kind() const override {
        return EConnectionKind::Merge;
    }

    void Resize(size_t srcSize, size_t dstSize) override {
        assert(dstSize == 1);
        Size_ = srcSize;
        FetchId_ = 0;
        NFinished_ = 0;
        Flags_.assign(Size_, 0);
        Outputs_.clear();
        Outputs_.reserve(Size_);
        for (size_t i = 0; i < Size_; ++i) {
            Outputs_.push_back(std::make_unique<TSPSC<TRowSet>>(MaxOutput));
        }
    }

    bool CanPush(int src_id) const override {
        return Outputs_[src_id]->CanPush();
    }

    void Finish(int src_id) override {
        if (Flags_[src_id] == 0) {
            NFinished_ ++;
        }
        Flags_[src_id] = 1;
    }

    void Push(int src_id, TRowSet&& rowSet) override {
        auto pushed = Outputs_[src_id]->TryPush(std::move(rowSet));
        assert(pushed);
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) override {
        assert(dst_id == 0);
        return EFetchState::NO_DATA;
    }

    size_t Size_;
    size_t FetchId_ = 0;
    size_t NFinished_ = 0;
    std::vector<int> Flags_;
    std::vector<std::unique_ptr<TSPSC<TRowSet>>> Outputs_;
};

struct THashShuffle : IConnection {
    EConnectionKind Kind() const override {
        return EConnectionKind::HashShuffle;
    }

    void Resize(size_t srcSize, size_t dstSize) override {
        SrcSize_ = srcSize;
        DstSize_ = dstSize;
        NFinished_ = 0;
        FetchIds_.assign(DstSize_, 0);
        Flags_.assign(SrcSize_, 0);
        Outputs_.clear();
        Outputs_.reserve(SrcSize_);
        for (size_t src = 0; src < SrcSize_; ++src) {
            Outputs_.emplace_back();
            auto& output = Outputs_.back();
            output.reserve(DstSize_);
            for (size_t dst = 0; dst < DstSize_; ++dst) {
                output.push_back(std::make_unique<TSPSC<TRowSet>>(MaxOutput));
            }
        }
    }

    bool CanPush(int src_id) const override {
        // return Outputs_[src_id].size() < MaxOutput;
        // TODO: check size in bytes
        return true;
    }

    void Finish(int src_id) override {
        if (Flags_[src_id] == 0) {
            NFinished_ ++;
        }
        Flags_[src_id] = 1;
    }

    void Push(int src_id, TRowSet&& rowSet) override {
        // compute hash => distribute accrose outputs
        // hash() % DstSize
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) override {
        auto from = (FetchIds_[dst_id] + 1) % SrcSize_;
        FetchIds_[dst_id] = from;
        for (size_t i = 0; i < SrcSize_; ++i) {
            auto index = (from + i) % SrcSize_;
            if (Outputs_[index][dst_id]->TryPop(batch)) {
                return EFetchState::OK;
            }
        }

        return NFinished_ == SrcSize_
            ? EFetchState::FINISHED
            : EFetchState::NO_DATA;
    }

    size_t SrcSize_;
    size_t DstSize_;
    size_t NFinished_ = 0;

    // FetchIds_.size() == DstSize_
    std::vector<size_t> FetchIds_; // src id to fetch start from
    std::vector<int> Flags_;

    // src_id -> dst_id -> queue
    std::vector<std::vector<std::unique_ptr<TSPSC<TRowSet>>>> Outputs_;
};

// this is not IRuntimeNode from qdb
// this should be called smth like ITask maybe?
// this should hold light-weight node with kernel+dispatch logic (w/o state), state, params
// e.g. this runtime node efficientely is qdb::IRuntimeNode+task/shard-local mutable state+task/shard-local mutable params
struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;

    virtual EState Execute() = 0;

    virtual bool IsFinished() const = 0;
    virtual void Run(TRowSet& rowSet) = 0;

    // connections
    virtual void SetInputPort(TInputPort inputPort) = 0;
    virtual void SetOutputPort(TOutputPort outputPort) = 0;

    // hack
    // for real use we need to create this task node on partition state
    // then wrap qdb::IRuntimeNode + attach state+params + attach PartitionId + attach InputId
    //
    virtual IRuntimeNode* Clone() = 0;
};

struct TDagNode {
    std::vector<TDagNode*> Inbound;
    std::vector<TDagNode*> Outbound;
    IRuntimeNode* Compute;
    int Partitions = 1;
    std::atomic<ETaskState> State = ETaskState::Idle;
};

template <class T>
class TMPMCQueue {
public:
    explicit TMPMCQueue(size_t capacity)
        : Capacity_(capacity)
        , Buffer_(Capacity_)
    {
        assert(Capacity_ > 0);
        for (size_t i = 0; i < Capacity_; ++i) {
            Buffer_[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool TryPush(T value) {
        auto pos = PushPos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = Buffer_[pos % Capacity_];
            auto seq = cell.Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (PushPos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    cell.Value = value;
                    cell.Sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = PushPos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPop(T& value) {
        auto pos = PopPos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = Buffer_[pos % Capacity_];
            auto seq = cell.Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (PopPos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    value = cell.Value;
                    cell.Sequence.store(pos + Capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = PopPos_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct TCell {
        std::atomic<size_t> Sequence = 0;
        T Value = {};
    };

    size_t Capacity_;
    std::vector<TCell> Buffer_;
    std::atomic<size_t> PushPos_ = 0;
    std::atomic<size_t> PopPos_ = 0;
};

struct TEdgeKey {
    IRuntimeNode* Src = nullptr;
    IRuntimeNode* Dst = nullptr;

    bool operator==(const TEdgeKey& other) const {
        return Src == other.Src && Dst == other.Dst;
    }
};

struct TEdgeKeyHash {
    size_t operator()(const TEdgeKey& key) const {
        auto src = std::hash<IRuntimeNode*>{}(key.Src);
        auto dst = std::hash<IRuntimeNode*>{}(key.Dst);
        return src ^ (dst + 0x9e3779b97f4a7c15ULL + (src << 6) + (src >> 2));
    }
};

struct TGraph {
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Inbound;
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Outbound;
    std::unordered_map<TEdgeKey, std::unique_ptr<IConnection>, TEdgeKeyHash> Connections;

    std::vector<std::unique_ptr<IRuntimeNode>> OwnedNodes;
    std::vector<std::unique_ptr<TDagNode>> DagNodes;
    std::unordered_map<IRuntimeNode*, TDagNode*> NodeToDagNode;
    TDagNode* Root = nullptr;
    std::vector<TDagNode*> Leaves;
    int Size = 0;

    void Build() {
        DagNodes.clear();
        NodeToDagNode.clear();
        Root = nullptr;
        Leaves.clear();

        for (auto& [dst, _] : Inbound) {
            auto dagNode = std::make_unique<TDagNode>();
            NodeToDagNode[dst] = dagNode.get();
            dagNode->Compute = dst;

            if (!Outbound.contains(dst)) {
                Root = dagNode.get();
            }

            DagNodes.emplace_back(std::move(dagNode));
        }

        for (auto& [src, _] : Outbound) {
            if (NodeToDagNode.contains(src)) {
                continue;
            }

            auto dagNode = std::make_unique<TDagNode>();
            dagNode->Compute = src;
            NodeToDagNode[src] = dagNode.get();

            if (!Inbound.contains(src)) {
                Leaves.push_back(dagNode.get());
            }

            DagNodes.emplace_back(std::move(dagNode));
        }

        for (auto& [dst, srcs] : Inbound) {
            auto node = NodeToDagNode[dst];
            for (auto src : srcs) {
                auto srcNode = NodeToDagNode[src];
                assert(srcNode);
                node->Inbound.emplace_back(srcNode);
            }
        }

        for (auto& [src, dsts] : Outbound) {
            auto node = NodeToDagNode[src];
            for (auto dst : dsts) {
                auto dstNode = NodeToDagNode[dst];
                node->Outbound.emplace_back(dstNode);
            }
        }

        Size = DagNodes.size();
    }

    IConnection& GetConnection(IRuntimeNode* src, IRuntimeNode* dst) const {
        auto it = Connections.find({src, dst});
        assert(it != Connections.end());
        return *it->second;
    }

    std::unique_ptr<IConnection> MakeConnection(EConnectionKind kind) const {
        switch (kind) {
        case EConnectionKind::OneToOne:
            return std::make_unique<TOneToOne>();
        case EConnectionKind::Gather:
            return std::make_unique<TGather>();
        default:
            assert(false);
            return {};
        }
    }

    void InferPartitions() {
        std::unordered_set<TDagNode*> visited;
        for (auto node : Leaves) {
            InferPartitions(node, visited);
        }
    }

    void InferPartitions(TDagNode* node, std::unordered_set<TDagNode*>& visited) {
        if (visited.contains(node)) {
            return;
        }
        visited.emplace(node);

        if (node->Inbound.empty()) {
            if (node->Partitions <= 1) {
                node->Partitions = 2;
            }
        } else {
            bool gathersInput = false;
            int partitions = 1;
            for (auto in : node->Inbound) {
                auto& conn = GetConnection(in->Compute, node->Compute);
                if (conn.Kind() == EConnectionKind::Gather ||
                    conn.Kind() == EConnectionKind::Merge) {
                    gathersInput = true;
                } else {
                    partitions = std::max(partitions, in->Partitions);
                }
            }
            if (node->Partitions <= 1) {
                node->Partitions = gathersInput ? 1 : partitions;
            }
        }

        for (auto out : node->Outbound) {
            InferPartitions(out, visited);
        }
    }

    std::unique_ptr<TGraph> Partition() {
        InferPartitions();

        auto out = std::make_unique<TGraph>();
        std::unordered_map<TDagNode*, std::vector<IRuntimeNode*>> clones;
        for (auto& node : DagNodes) {
            auto& nodeClones = clones[node.get()];
            nodeClones.reserve(node->Partitions);
            for (int i = 0; i < node->Partitions; ++i) {
                std::unique_ptr<IRuntimeNode> clone(node->Compute->Clone());
                nodeClones.push_back(clone.get());
                out->OwnedNodes.emplace_back(std::move(clone));
            }
        }

        for (auto& [edge, conn] : Connections) {
            auto srcNode = NodeToDagNode[edge.Src];
            auto dstNode = NodeToDagNode[edge.Dst];
            auto& srcClones = clones[srcNode];
            auto& dstClones = clones[dstNode];

            auto newConn = MakeConnection(conn->Kind());
            newConn->Resize(srcClones.size(), dstClones.size());
            auto* connPtr = newConn.get();

            if (conn->Kind() == EConnectionKind::OneToOne) {
                assert(srcClones.size() == dstClones.size());
                for (size_t i = 0; i < srcClones.size(); ++i) {
                    srcClones[i]->SetOutputPort(TOutputPort(connPtr, static_cast<int>(i)));
                    dstClones[i]->SetInputPort(TInputPort(connPtr, static_cast<int>(i)));
                    out->Outbound[srcClones[i]].push_back(dstClones[i]);
                    out->Inbound[dstClones[i]].push_back(srcClones[i]);
                }
            } else if (conn->Kind() == EConnectionKind::Gather) {
                assert(dstClones.size() == 1);
                for (size_t i = 0; i < srcClones.size(); ++i) {
                    srcClones[i]->SetOutputPort(TOutputPort(connPtr, static_cast<int>(i)));
                    out->Outbound[srcClones[i]].push_back(dstClones[0]);
                    out->Inbound[dstClones[0]].push_back(srcClones[i]);
                }
                dstClones[0]->SetInputPort(TInputPort(connPtr, 0));
            } else {
                assert(false);
            }

            out->Connections.emplace(
                TEdgeKey{srcClones.front(), dstClones.front()}, std::move(newConn));
        }

        out->Build();
        return out;
    }

    void Connect(IRuntimeNode* src, IRuntimeNode* dst, EConnectionKind kind)
    {
        Inbound[dst].push_back(src);
        Outbound[src].push_back(dst);

        switch (kind) {
        case EConnectionKind::OneToOne: {
            auto conn = std::make_unique<TOneToOne>();
            src->SetOutputPort(TOutputPort(conn.get(), 0));
            dst->SetInputPort(TInputPort(conn.get(), 0));
            Connections.emplace(TEdgeKey{src, dst}, std::move(conn));
            break;
        }
        case EConnectionKind::Gather: {
            auto conn = std::make_unique<TGather>();
            src->SetOutputPort(TOutputPort(conn.get(), 0));
            dst->SetInputPort(TInputPort(conn.get(), 0));
            Connections.emplace(TEdgeKey{src, dst}, std::move(conn));
            break;
        }
        default:
            assert(false);
            break;
        };
    }
};

struct TNode : virtual IRuntimeNode {
    TInputPort InputPort_;
    TOutputPort OutputPort_;

    // connection
    void SetInputPort(TInputPort inputPort) override {
        InputPort_ = std::move(inputPort);
    }

    void SetOutputPort(TOutputPort outputPort) override {
        OutputPort_ = std::move(outputPort);
    }
};

struct TNodeWithInput : TNode {
    TNodeWithInput()
    { }

    bool IsFinished() const override {
        return InputConsumed_;
    }

    EState Execute() override {
        TRowSet rowSet;
        if (!CurrentInput_) {
            auto state = InputPort_.Fetch(rowSet);
            if (state == EFetchState::OK) {
                CurrentInput_.emplace(std::move(rowSet));
            } else if (state == EFetchState::FINISHED) {
                InputConsumed_ = true;
            }
        }

        if (!CurrentInput_) {
            if (InputConsumed_) {
                OutputPort_.Finish();
                return EState::FINISHED;
            }
            return EState::NEED_DATA;
        }

        if (!OutputPort_.CanPush()) {
            return EState::BLOCKED_OUTPUT;
        }

        rowSet = std::move(*CurrentInput_); CurrentInput_ = std::nullopt;
        Run(rowSet);
        OutputPort_.Push(std::move(rowSet));

        if (InputConsumed_) {
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        return EState::OK;
    }

    bool InputConsumed_ = false;
    std::optional<TRowSet> CurrentInput_;
};

struct TScan : TNode {
    int NSets;
    int Seed;

    int Ncols = 16;

    int SetId = 0;

    TScan(int nsets, int seed)
        : NSets(nsets)
        , Seed(seed)
    { }

    bool IsFinished() const override {
        return SetId == NSets;
    }

    void Run(TRowSet& rowSet) override {
        GenerateRowSet(rowSet, Ncols, &Seed);
        SetId ++;
    }

    EState Execute() override {
        if (IsFinished()) {
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        if (!OutputPort_.CanPush()) {
            return EState::BLOCKED_OUTPUT;
        }
        TRowSet rowSet;
        Run(rowSet);
        OutputPort_.Push(std::move(rowSet));

        if (IsFinished()) {
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        return EState::OK;
    }

    IRuntimeNode* Clone() override {
        return new TScan(NSets, Seed);
    }
};

struct TFilter : TNodeWithInput {
    int Seed;
    int Percent;

    TFilter(int seed, int percent)
        : Seed(seed)
        , Percent(percent)
    { }

    void Run(TRowSet& rowSet) override {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                int k = (RandomInt(&Seed) % 100);
                if (k >= Percent) {
                    rowSet.Selection[i] = 0;
                }
            }
        }
    }

    IRuntimeNode* Clone() override {
        return new TFilter(Seed, Percent);
    }
};

struct TPrinter : TNodeWithInput {
    TPrinter()
    { }

    void Run(TRowSet& rowSet) override {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                std::cout << rowSet.Col[i] << "\n";
            }
        }
    }

    IRuntimeNode* Clone() override {
        return new TPrinter();
    }
};

struct TNullScheduler {
    bool Step(TDagNode* node, TRowSet& rowSet) {
        if (node->Inbound.empty()) {
            if (node->Compute->IsFinished()) {
                return false;
            }

            node->Compute->Run(rowSet);
            return true;
        }

        // assume have only 1 input node
        if (!Step(node->Inbound.front(), rowSet)) {
            return false;
        }

        node->Compute->Run(rowSet);
        return true;
    }

    void Run(TDagNode* root) {
        TRowSet rowSet;
        while (Step(root, rowSet)) {
        }
    }
};

struct TScheduler {
    TGraph& Graph;
    std::list<TDagNode*> Ready;
    std::unordered_set<TDagNode*> Scheduled;

    TScheduler(TGraph& g)
        : Graph(g)
    {
        Schedule(g.Root);
    }

    void Schedule(TDagNode* node) {
        if (!Scheduled.contains(node)) {
            Ready.emplace_back(node);
            Scheduled.emplace(node);
        }
    }

    void ScheduleInput(TDagNode* node) {
        for (auto in : node->Inbound) {
            Schedule(in);
        }
    }

    void ScheduleOutput(TDagNode* node) {
        for (auto out : node->Outbound) {
            Schedule(out);
        }
    }

    void Run() {
        while (!Ready.empty()) {
            auto node = Ready.front(); Ready.pop_front();
            Scheduled.erase(node);
            auto state = node->Compute->Execute();
            if (state == EState::NEED_DATA) {
                ScheduleInput(node);
            } else if (state == EState::BLOCKED_OUTPUT || state == EState::FINISHED) {
                ScheduleOutput(node);
            } else if (state == EState::OK) {
                Schedule(node);
                ScheduleOutput(node);
            }
        }
    }
};

struct TThreadedScheduler {
    TGraph& Graph;
    TMPMCQueue<TDagNode*> Ready;
    std::atomic<int> NFinished = 0;
    int NThreads;

    TThreadedScheduler(TGraph& g, int nthreads = 2)
        : Graph(g)
        , Ready(static_cast<size_t>(std::max(g.Size, 1)))
        , NThreads(nthreads)
    {
        Schedule(Graph.Root);
    }

    void Schedule(TDagNode* node) {
        auto state = node->State.load(std::memory_order_acquire);
        for (;;) {
            if (state == ETaskState::Idle) {
                if (node->State.compare_exchange_weak(
                        state,
                        ETaskState::Queued,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    break;
                }
            } else if (state == ETaskState::Running) {
                if (node->State.compare_exchange_weak(
                        state,
                        ETaskState::Reschedule,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return;
                }
            } else {
                return;
            }
        }

        // A worker preempted inside TryPop still holds its cell, so the ring
        // can be full while fewer than Graph.Size nodes are queued.
        while (!Ready.TryPush(node)) {
            std::this_thread::yield();
        }
    }

    TDagNode* PopReady() {
        TDagNode* node = nullptr;
        Ready.TryPop(node);
        return node;
    }

    void ScheduleInput(TDagNode* node) {
        for (auto in : node->Inbound) {
            Schedule(in);
        }
    }

    void ScheduleOutput(TDagNode* node) {
        for (auto out : node->Outbound) {
            Schedule(out);
        }
    }

    void Run() {
        std::vector<std::unique_ptr<std::thread>> threads;
        threads.reserve(NThreads);
        for (int i = 0; i < NThreads; ++i) {
            threads.emplace_back(
                std::make_unique<std::thread>([&](){ Work(); })
            );
        }
        for (int i = 0; i < NThreads; ++i) {
            threads[i]->join();
        }
    }

    void Work() {
        while (NFinished != Graph.Size) {
            auto node = PopReady();
            if (!node) {
                std::this_thread::yield();
                continue;
            }

            auto expected = ETaskState::Queued;
            if (!node->State.compare_exchange_strong(
                    expected,
                    ETaskState::Running,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;
            }

            auto state = node->Compute->Execute();

            if (state == EState::NEED_DATA) {
                auto reschedule = FinishRun(node);
                ScheduleInput(node);
                if (reschedule) {
                    Schedule(node);
                }
            } else if (state == EState::BLOCKED_OUTPUT) {
                auto reschedule = FinishRun(node);
                ScheduleOutput(node);
                if (reschedule) {
                    Schedule(node);
                }
            } else if (state == EState::FINISHED) {
                FinishNode(node);
                ScheduleOutput(node);
            } else if (state == EState::OK) {
                FinishRun(node);
                Schedule(node);
                ScheduleOutput(node);
            }
        }
    }

    bool FinishRun(TDagNode* node) {
        auto previous = node->State.exchange(ETaskState::Idle, std::memory_order_acq_rel);
        assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
        return previous == ETaskState::Reschedule;
    }

    void FinishNode(TDagNode* node) {
        auto previous = node->State.exchange(ETaskState::Finished, std::memory_order_acq_rel);
        assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
        ++NFinished;
    }
};

} // namespace

TEST(Scheduler, Basic) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30);
    auto printer = std::make_unique<TPrinter>();


    TGraph g;
    g.Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    g.Connect(filter.get(), printer.get(), EConnectionKind::OneToOne);
    g.Build();

    TNullScheduler scheduler;
    scheduler.Run(g.Root);
}

TEST(Scheduler, Async) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30);
    auto printer = std::make_unique<TPrinter>();

    TGraph g;
    g.Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    g.Connect(filter.get(), printer.get(), EConnectionKind::Gather);
    g.Build();

    auto pg = g.Partition();
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(pg->Leaves.size(), 2u);
    ASSERT_NE(pg->Root, nullptr);

    TScheduler scheduler(*pg);
    scheduler.Run();
}

TEST(Scheduler, AsyncThreads) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30);
    auto printer = std::make_unique<TPrinter>();

    TGraph g;
    g.Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    g.Connect(filter.get(), printer.get(), EConnectionKind::Gather);
    g.Build();

    auto pg = g.Partition();
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(pg->Leaves.size(), 2u);
    ASSERT_NE(pg->Root, nullptr);

    TThreadedScheduler scheduler(*pg);
    scheduler.Run();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
