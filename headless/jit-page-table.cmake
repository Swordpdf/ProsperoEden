# The JIT's direct page table. Eden packs each guest page's host pointer with its type and block
# number, so every JIT load/store decoded it (shl, sar, and) and tested the marked bit before the
# access. Each process page table now keeps a second array, DIRECT_TABLE_OFFSET bytes after its
# entries in the same sparse vector, holding per page the value the JIT adds to the guest address:
# the entry's pointer when the page is directly accessible, 0 when the access must take the
# callback path (marked, debug or unmapped). The JIT is given that array with no pointer mask,
# marked bit or sign extension, which dynarmic already supports (test, jz, access).
#
# Every write of an entry goes through PageEntryData (Store, MarkRasterizerCached, MarkDebug) or
# ZeroRegion on an unmap (core/memory.cpp, below in CMakeLists.txt); those write the direct value
# too: before the entry when the page becomes slow, after it when it becomes fast, so the JIT is
# never more permissive than it was with the entry alone. Committing a part of the entries
# commits its mirror (sparse_large_vector.h, mirror_index). The tables are only direct when
# Eden's tables are sparse here; dense tables would hold the mirror's 1 GiB in memory.
# dev-settings jit_table=off keeps the packed table for A/B runs.

file(READ "${PROJECT_SOURCE_DIR}/src/common/page_table.h" page_table_header)
function(eden_page_table_replace old new)
    string(FIND "${page_table_header}" "${old}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned page_table.h changed: '${old}'")
    endif()
    string(REPLACE "${old}" "${new}" changed "${page_table_header}")
    set(page_table_header "${changed}" PARENT_SCOPE)
endfunction()

eden_page_table_replace("    /// Specifies sign bit for page table entries.\n" [=[
    /// ProsperoEden: the JIT's direct table (headless/jit-page-table.cmake) starts this many bytes
    /// after the entries: 1 GiB, the entries of a 39-bit address space.
    static constexpr std::size_t DIRECT_TABLE_OFFSET = std::size_t{1} << 30;
    /// Whether process page tables keep a direct table: -1 until the first table is sized, then
    /// fixed for the session (every PageEntryData write relies on it).
    static inline std::atomic<int> direct_tables{-1};
    /// dev-settings jit_table=off, read when the first table is sized.
    static inline std::atomic<bool> direct_tables_requested{true};
    static bool DirectTables() noexcept;

    /// Specifies sign bit for page table entries.
]=])

eden_page_table_replace([=[        /// Write page info atomically
        constexpr void Store(bool marked, PageType type, u16 block, uintptr_t pointer) noexcept {
            data_raw.store(std::bit_cast<u64>(Data{marked, type, block, pointer}));
        }

        constexpr void MarkRasterizerCached() noexcept {
            data_raw.fetch_or(0b111);
        }
]=] [=[        /// Write page info atomically, and the JIT's direct value: before the entry when the page
        /// becomes slow, after it when it becomes fast.
        void Store(bool marked, PageType type, u16 block, uintptr_t pointer) noexcept {
            const Data data{marked, type, block, pointer};
            const uintptr_t direct = ExtractPointer(data);
            const bool mirrored = direct_tables.load(std::memory_order_relaxed) == 1;
            if (mirrored && direct == 0) Direct().store(0, std::memory_order_relaxed);
            data_raw.store(std::bit_cast<u64>(data));
            if (mirrored && direct != 0) Direct().store(direct, std::memory_order_relaxed);
        }

        void MarkRasterizerCached() noexcept {
            if (direct_tables.load(std::memory_order_relaxed) == 1) Direct().store(0, std::memory_order_relaxed);
            data_raw.fetch_or(0b111);
        }
]=])

eden_page_table_replace([=[    private:
        std::atomic<u64> data_raw;
]=] [=[    private:
        std::atomic<u64>& Direct() noexcept {
            return *reinterpret_cast<std::atomic<u64>*>(reinterpret_cast<char*>(&data_raw) + DIRECT_TABLE_OFFSET);
        }
        std::atomic<u64> data_raw;
]=])

eden_page_table_replace([=[    std::size_t GetAddressSpaceBits() const {
        return current_address_space_width_in_bits;
    }
]=] [=[    std::size_t GetAddressSpaceBits() const {
        return current_address_space_width_in_bits;
    }

    /// The JIT's direct table, or null when this table has none.
    void* DirectTable() noexcept {
        return entries.mirror_index ? const_cast<PageEntryData*>(entries.data()) + entries.mirror_index : nullptr;
    }
]=])
write_derived("${PORT_BUILD_DIR}/sparse/common/page_table.h" "${page_table_header}")

file(READ "${PROJECT_SOURCE_DIR}/src/common/page_table.cpp" page_table_unit)
set(resize_old "    entries.ResizeAndClear(num_page_table_entries);\n")
string(FIND "${page_table_unit}" "${resize_old}" resize_at)
if(resize_at LESS 0)
    message(FATAL_ERROR "Pinned PageTable::Resize changed")
endif()
string(REPLACE "${resize_old}" [=[    if (DirectTables()) {
        ASSERT(num_page_table_entries * sizeof(PageEntryData) <= DIRECT_TABLE_OFFSET);
        constexpr std::size_t mirror = DIRECT_TABLE_OFFSET / sizeof(PageEntryData);
        entries.ResizeAndClear(mirror + num_page_table_entries);
        entries.mirror_index = mirror;
    } else {
        entries.ResizeAndClear(num_page_table_entries);
        entries.mirror_index = 0;
    }
]=] page_table_unit "${page_table_unit}")
string(REPLACE "namespace Common {\n" [=[namespace Common {

bool PageTable::DirectTables() noexcept {
    int mode = direct_tables.load(std::memory_order_acquire);
    if (mode < 0) {
        static std::once_flag once;
        std::call_once(once, [] {
            const bool direct = direct_tables_requested.load() && SparseTablesAvailable();
            direct_tables.store(direct ? 1 : 0, std::memory_order_release);
            std::printf("EDEN_JIT_TABLE direct=%d\n", direct ? 1 : 0);
        });
        mode = direct_tables.load(std::memory_order_acquire);
    }
    return mode == 1;
}
]=] page_table_unit "${page_table_unit}")
write_derived("${PORT_BUILD_DIR}/page_table.cpp" "#include <cstdio>\n#include <mutex>\n#include \"common/assert.h\"\n${page_table_unit}")
get_target_property(common_sources common SOURCES)
list(REMOVE_ITEM common_sources page_table.cpp)
list(APPEND common_sources "${PORT_BUILD_DIR}/page_table.cpp")
set_property(TARGET common PROPERTY SOURCES "${common_sources}")
