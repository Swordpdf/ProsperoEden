// ProsperoEden: one page lookup for nearby A64 accesses (headless/dynarmic/page-reuse.cmake).
// Accesses whose addresses are one IR value plus constants, within 256 bytes and with no
// exclusive, ordered or exception-raising operation between them, share an A64EdenPageLookup:
// the JIT direct table's value for that page, or 0 when the group does not fit one page or the
// page must take the callback path. The lookup replaces each access's location argument (only
// read with check_halt_on_memory_access, which keeps this pass off).
extern "C" {
// dev-settings jit_page_reuse=off; read when a block is compiled.
bool eden_page_reuse = true;
// Groups formed and the accesses they hold, counted while blocks are compiled.
unsigned long long eden_page_reuse_groups = 0;
unsigned long long eden_page_reuse_accesses = 0;
}

namespace {

constexpr s64 eden_reuse_span = 256;

struct EdenAddress {
    IR::Inst* root;
    s64 offset;
};

std::optional<EdenAddress> EdenDecomposeAddress(IR::Value value) {
    s64 offset = 0;
    for (int depth = 0; depth < 8; ++depth) {
        if (value.IsImmediate()) {
            return std::nullopt;
        }
        IR::Inst* const inst = value.GetInst();
        const IR::Opcode op = inst->GetOpcode();
        if (op == IR::Opcode::Identity) {
            value = inst->GetArg(0);
            continue;
        }
        if ((op == IR::Opcode::Add64 || op == IR::Opcode::Sub64) && inst->GetArg(2).IsImmediate() && !inst->GetArg(2).GetU1()) {
            const bool right = inst->GetArg(1).IsImmediate();
            const bool left = op == IR::Opcode::Add64 && inst->GetArg(0).IsImmediate();
            if (right || left) {
                const s64 constant = static_cast<s64>(inst->GetArg(right ? 1 : 0).GetImmediateAsU64());
                if (constant < -(s64{1} << 20) || constant > (s64{1} << 20)) {
                    return EdenAddress{inst, offset};
                }
                offset += op == IR::Opcode::Add64 ? constant : -constant;
                value = inst->GetArg(right ? 0 : 1);
                continue;
            }
        }
        return EdenAddress{inst, offset};
    }
    return std::nullopt;
}

// Bytes accessed, or 0 for an operation this pass leaves alone.
size_t EdenReusableBytes(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::A64ReadMemory8: case IR::Opcode::A64WriteMemory8: return 1;
    case IR::Opcode::A64ReadMemory16: case IR::Opcode::A64WriteMemory16: return 2;
    case IR::Opcode::A64ReadMemory32: case IR::Opcode::A64WriteMemory32: return 4;
    case IR::Opcode::A64ReadMemory64: case IR::Opcode::A64WriteMemory64: return 8;
    case IR::Opcode::A64ReadMemory128: case IR::Opcode::A64WriteMemory128: return 16;
    default: return 0;
    }
}

bool EdenIsWrite(IR::Opcode op) {
    return op == IR::Opcode::A64WriteMemory8 || op == IR::Opcode::A64WriteMemory16 || op == IR::Opcode::A64WriteMemory32 ||
           op == IR::Opcode::A64WriteMemory64 || op == IR::Opcode::A64WriteMemory128;
}

// Operations after which a group's lookup is not reused: exclusive accesses and monitor
// changes, barriers, supervisor calls, exceptions and cache maintenance.
bool EdenEndsGroups(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A64CallSupervisor:
    case IR::Opcode::A64ExceptionRaised:
    case IR::Opcode::A64DataCacheOperationRaised:
    case IR::Opcode::A64InstructionCacheOperationRaised:
    case IR::Opcode::A64DataMemoryBarrier:
    case IR::Opcode::A64DataSynchronizationBarrier:
    case IR::Opcode::A64InstructionSynchronizationBarrier:
    case IR::Opcode::A64ClearExclusive:
    case IR::Opcode::A64ExclusiveReadMemory8:
    case IR::Opcode::A64ExclusiveReadMemory16:
    case IR::Opcode::A64ExclusiveReadMemory32:
    case IR::Opcode::A64ExclusiveReadMemory64:
    case IR::Opcode::A64ExclusiveReadMemory128:
    case IR::Opcode::A64ExclusiveWriteMemory8:
    case IR::Opcode::A64ExclusiveWriteMemory16:
    case IR::Opcode::A64ExclusiveWriteMemory32:
    case IR::Opcode::A64ExclusiveWriteMemory64:
    case IR::Opcode::A64ExclusiveWriteMemory128:
        return true;
    default:
        return false;
    }
}

}  // namespace

static void EdenPageReusePass(IR::Block& block, const A64::UserConfig& conf) {
    // The emitted lookup reads the direct table: absolute offsets, no attributes, 8-byte
    // entries, an address space that is range-checked (not mirrored) and fits 32 index bits.
    if (!eden_page_reuse || !conf.page_table || conf.fastmem_pointer || conf.check_halt_on_memory_access ||
        !conf.absolute_offset_page_table || conf.page_table_pointer_mask != 0 || conf.page_table_marked_bit ||
        conf.page_table_sign_extension || conf.silently_mirror_page_table || conf.page_table_log2_stride != 3 ||
        conf.page_table_address_space_bits <= 12 || conf.page_table_address_space_bits > 42 ||
        !conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        return;
    }
    struct Member {
        IR::Block::iterator at;
        s64 offset;
    };
    struct Group {
        IR::Inst* root;
        s64 low, high;
        std::vector<Member> members;
    };
    std::vector<Group> closed;
    std::vector<Group> open;
    const auto close_all = [&] {
        for (auto& group : open)
            closed.push_back(std::move(group));
        open.clear();
    };
    for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it) {
        IR::Inst& inst = *it;
        if (EdenEndsGroups(inst.GetOpcode())) {
            close_all();
            continue;
        }
        const size_t bytes = EdenReusableBytes(inst);
        if (bytes == 0 || !inst.GetArg(0).IsImmediate()) {
            continue;
        }
        const IR::AccType type = inst.GetArg(EdenIsWrite(inst.GetOpcode()) ? 3 : 2).GetAccType();
        if (type != IR::AccType::NORMAL && type != IR::AccType::VEC && type != IR::AccType::UNPRIV) {
            close_all();
            continue;
        }
        const auto address = EdenDecomposeAddress(inst.GetArg(1));
        if (!address) {
            continue;
        }
        const s64 end = address->offset + static_cast<s64>(bytes);
        auto group = std::find_if(open.begin(), open.end(), [&](const Group& g) { return g.root == address->root; });
        if (group != open.end()) {
            const s64 low = std::min(group->low, address->offset), high = std::max(group->high, end);
            if (high - low <= eden_reuse_span) {
                group->low = low;
                group->high = high;
                group->members.push_back({it, address->offset});
                continue;
            }
            closed.push_back(std::move(*group));
            open.erase(group);
        }
        open.push_back({address->root, address->offset, end, {{it, address->offset}}});
    }
    close_all();
    unsigned long long groups = 0, accesses = 0;
    for (const Group& group : closed) {
        if (group.members.size() < 2) {
            continue;
        }
        const IR::Block::iterator first = group.members.front().at;
        IR::Value low{group.root};
        if (group.low != 0) {
            low = IR::Value{&*block.PrependNewInst(first, IR::Opcode::Add64,
                {IR::Value{group.root}, IR::Value{static_cast<u64>(group.low)}, IR::Value{false}})};
        }
        IR::Inst* const lookup = &*block.PrependNewInst(first, IR::Opcode::A64EdenPageLookup,
            {low, IR::Value{static_cast<u64>(group.high - group.low)}});
        for (const Member& member : group.members)
            member.at->SetArg(0, IR::Value{lookup});
        ++groups;
        accesses += group.members.size();
    }
    if (groups != 0) {
        __atomic_fetch_add(&eden_page_reuse_groups, groups, __ATOMIC_RELAXED);
        __atomic_fetch_add(&eden_page_reuse_accesses, accesses, __ATOMIC_RELAXED);
    }
}
