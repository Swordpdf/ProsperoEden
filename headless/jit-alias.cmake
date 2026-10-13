# Keep instruction addresses in the executable view; redirect only byte writes.
set(xbyak_root "${PROJECT_SOURCE_DIR}/.cache/cpm/xbyak/v7.40.1/xbyak")
file(READ "${xbyak_root}/xbyak.h" xbyak_header)
string(REPLACE "virtual void free(uint8_t *p) { AlignedFree(p); }"
    "virtual uint8_t *writableAddress(uint8_t *p) { return p; }\n\tvirtual void commit(uint8_t *, size_t) {}\n\tvirtual void free(uint8_t *p) { AlignedFree(p); }" xbyak_header "${xbyak_header}")
string(REPLACE "uint8_t *top_;" "uint8_t *top_;\n\tuint8_t *writeTop_;" xbyak_header "${xbyak_header}")
string(REPLACE ", size_(0)" ", writeTop_(type_ == USER_BUF ? top_ : alloc_->writableAddress(top_))\n\t\t, size_(0)" xbyak_header "${xbyak_header}")
string(REPLACE "newTop[i] = top_[i]" "alloc_->writableAddress(newTop)[i] = top_[i]" xbyak_header "${xbyak_header}")
string(REPLACE "top_ = newTop;" "top_ = newTop;\n\t\twriteTop_ = alloc_->writableAddress(top_);" xbyak_header "${xbyak_header}")
string(REPLACE "top_[size_++] =" "writeTop_[size_++] =" xbyak_header "${xbyak_header}")
string(REPLACE "uint8_t *const data = top_ + offset;" "uint8_t *const data = writeTop_ + offset;" xbyak_header "${xbyak_header}")
string(REPLACE "const uint8_t *getCode() const { return top_; }"
    "bool hasWritableAlias() const { return writeTop_ != top_; }\n\tvoid commitMemory(size_t bytes) { if (type_ != USER_BUF) alloc_->commit(top_, bytes); }\n\tuint8_t *writableAddress(const void *p) const { return writeTop_ + (static_cast<const uint8_t*>(p) - top_); }\n\tconst uint8_t *getCode() const { return top_; }" xbyak_header "${xbyak_header}")
write_derived("${PORT_BUILD_DIR}/include/xbyak/xbyak.h" "${xbyak_header}")
# xbyak_util includes its sibling header: keep every consumer on the same layout.
foreach(sibling xbyak_mnemonic.h xbyak_util.h)
    configure_file("${xbyak_root}/${sibling}" "${PORT_BUILD_DIR}/include/xbyak/${sibling}" COPYONLY)
endforeach()
target_include_directories(common BEFORE PUBLIC "${PORT_BUILD_DIR}/include")
# Existing Ninja dependencies may still name the original, now-shadowed header.
# Change consumer commands as well so no old CodeArray layout survives incrementally.
target_compile_definitions(dynarmic PUBLIC EDEN_JIT_ALIAS_LAYOUT=1)

string(REPLACE "    memset(ret, 0, alloc_size);" "    memset(writableAddress(ret), 0, alloc_size);" code_source "${code_source}")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/constant_pool.cpp" pool_source)
string(REPLACE "        target_constant = constant;"
    "        *reinterpret_cast<ConstantT*>(code.writableAddress(&target_constant)) = constant;" pool_source "${pool_source}")
write_derived("${PORT_BUILD_DIR}/constant_pool.cpp" "${pool_source}")
get_target_property(alias_sources dynarmic SOURCES)
list(FILTER alias_sources EXCLUDE REGEX "(^|/)constant_pool\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${alias_sources}")
target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/constant_pool.cpp")
