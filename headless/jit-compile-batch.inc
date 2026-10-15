        // Publish bounded, unconditional chains together without compiling
        // either unselected side of a conditional branch.
        // Do not follow indirect branches, supervisor calls or halt terminals.
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        struct Publish {
            BlockOfCode& code;
            ~Publish() { code.DisableWriting(); }
        } publish{block_of_code};
        CodePtr first = nullptr;
        std::array<std::optional<IR::LocationDescriptor>, 32> pending{};
        pending[0] = descriptor;
        unsigned queued = 1, batch = 0;
        for (unsigned cursor = 0; cursor < queued && batch < 16; ++cursor) {
            descriptor = *pending[cursor];
            if (cursor && (emitter.GetBasicBlock(descriptor) ||
                           !get_code(A64::LocationDescriptor{descriptor}.PC())))
                continue;
            if (batch && block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE)
                break;
            // A cache that takes its memory as it fills (src/memory_pages.cpp) asks before every block.
            if (batch) block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
            // Count additional batched blocks.
            const auto arch_descriptor = A64::LocationDescriptor{descriptor};
            ir_block.Reset(arch_descriptor);
            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
            Optimization::Optimize(ir_block, conf, polyfill_options);
            const auto entrypoint = emitter.Emit(ir_block, false).entrypoint;
            if (!first) first = entrypoint;
            ++batch;
            if (arch_descriptor.SingleStepping() || block_of_code.hasWritableAlias()) break;
            const auto queue = [&](const IR::Term::LeafTerminal& leaf) {
                const IR::LocationDescriptor* next = nullptr;
                if (const auto* link = std::get_if<IR::Term::LinkBlock>(&leaf)) next = &link->next;
                if (const auto* link = std::get_if<IR::Term::LinkBlockFast>(&leaf)) next = &link->next;
                if (!next || queued == pending.size() || emitter.GetBasicBlock(*next)) return;
                for (unsigned i = 0; i < queued; ++i)
                    if (*pending[i] == *next) return;
                pending[queued++] = *next;
            };
            if (const auto* leaf = std::get_if<IR::Term::LeafTerminal>(&ir_block.terminal)) queue(*leaf);
        }
        return first;
