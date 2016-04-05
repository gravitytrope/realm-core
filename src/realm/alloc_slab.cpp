#include <type_traits>
#include <exception>
#include <algorithm>
#include <memory>
#include <iostream>
#include <mutex>
#include <map>

#ifdef REALM_SLAB_ALLOC_DEBUG
#  include <cstdlib>
#endif

#include <realm/util/encrypted_file_mapping.hpp>
#include <realm/util/miscellaneous.hpp>
#include <realm/util/terminate.hpp>
#include <realm/util/thread.hpp>
#include <realm/array.hpp>
#include <realm/alloc_slab.hpp>

using namespace realm;
using namespace realm::util;


namespace {

#ifdef REALM_SLAB_ALLOC_DEBUG
std::map<ref_type, void*> malloc_debug_map;
#endif

class InvalidFreeSpace: std::exception {
public:
    const char* what() const noexcept override
    {
        return "Free space tracking was lost due to out-of-memory";
    }
};

} // anonymous namespace


struct SlabAlloc::MappedFile {

    util::Mutex m_mutex;
    util::File m_file;
    util::File::Map<char> m_initial_mapping;
    // additional sections beyond those covered by the initial mapping, are
    // managed as separate mmap allocations, each covering one section.
    size_t m_first_additional_mapping = 0;
    size_t m_num_additional_mappings = 0;
    size_t m_capacity_additional_mappings = 0;
    std::unique_ptr<util::File::Map<char>[]> m_additional_mappings;

    /// Version obtained when initially attaching the file.
    int m_file_format_version;

    /// If a file or buffer is currently attached and validation was
    /// not skipped during attachement, this flag is true if, and only
    /// if the attached file has a footer specifying the top-ref, that
    /// is, if the file is on the streaming form. 
    bool m_file_on_streaming_form = false;

    /// Indicates if attaching to the file was succesfull
    bool m_success = false;

    ~MappedFile() 
    {
        m_initial_mapping.unmap();
        // running the destructors on the mappings will cause them to unmap:
        m_additional_mappings = nullptr;
        m_file.close();
    }
};


SlabAlloc::SlabAlloc()
{
    m_initial_section_size = page_size();
    m_section_shifts = log2(m_initial_section_size);
    size_t max = std::numeric_limits<size_t>::max();
    m_num_section_bases = 1 + get_section_index(max);
    m_section_bases.reset( new size_t[m_num_section_bases] );
    for (size_t i = 0; i < m_num_section_bases; ++i) {
        m_section_bases[i] = compute_section_base(i);
    }
}

util::File& SlabAlloc::get_file()
{
    return m_file_mappings->m_file;
}


const SlabAlloc::Header SlabAlloc::empty_file_header = {
    { 0, 0 }, // top-refs
    { 'T', '-', 'D', 'B' },
    { 0, 0 }, // undecided file format
    0, // reserved
    0  // flags (lsb is select bit)
};


void SlabAlloc::init_streaming_header(Header* streaming_header, int file_format_version)
{
    using storage_type = std::remove_reference<decltype(Header::m_file_format[0])>::type;
    REALM_ASSERT(!util::int_cast_has_overflow<storage_type>(file_format_version));
    *streaming_header = {
        { 0xFFFFFFFFFFFFFFFFULL, 0 }, // top-refs
        { 'T', '-', 'D', 'B' },
        { storage_type(file_format_version), 0 },
        0, // reserved
        0  // flags (lsb is select bit)
    };
}


class SlabAlloc::ChunkRefEq {
public:
    ChunkRefEq(ref_type ref) noexcept:
        m_ref(ref)
    {
    }
    bool operator()(const Chunk& chunk) const noexcept
    {
        return chunk.ref == m_ref;
    }
private:
    ref_type m_ref;
};


class SlabAlloc::ChunkRefEndEq {
public:
    ChunkRefEndEq(ref_type ref) noexcept:
        m_ref(ref)
    {
    }
    bool operator()(const Chunk& chunk) const noexcept
    {
        return chunk.ref + chunk.size == m_ref;
    }
private:
    ref_type m_ref;
};


class SlabAlloc::SlabRefEndEq {
public:
    SlabRefEndEq(ref_type ref) noexcept:
        m_ref(ref)
    {
    }
    bool operator()(const Slab& slab) const noexcept
    {
        return slab.ref_end == m_ref;
    }
private:
    ref_type m_ref;
};


void SlabAlloc::detach() noexcept
{
    switch (m_attach_mode) {
        case attach_None:
        case attach_UsersBuffer:
            goto found;
        case attach_OwnedBuffer:
            ::free(m_data);
            goto found;
        case attach_SharedFile:
        case attach_UnsharedFile:
            m_data = 0;
            m_file_mappings = nullptr;
            goto found;
    }
    REALM_ASSERT(false);
  found:
    invalidate_cache();
    m_attach_mode = attach_None;
}


SlabAlloc::~SlabAlloc() noexcept
{
#ifdef REALM_DEBUG
    if (is_attached()) {
        // A shared group does not guarantee that all space is free
        if (m_attach_mode != attach_SharedFile) {
            // No point inchecking if free space info is invalid
            if (m_free_space_state != free_space_Invalid) {
                if (!is_all_free()) {
                    print();
#  ifndef REALM_SLAB_ALLOC_DEBUG
                    std::cerr << "To get the stack-traces of the corresponding allocations,"
                        "first compile with REALM_SLAB_ALLOC_DEBUG defined,"
                        "then run under Valgrind with --leak-check=full\n";
                    REALM_TERMINATE("SlabAlloc detected a leak");
#  endif
                }
            }
        }
    }
#endif

    // Release all allocated memory
    for (auto& slab : m_slabs) {
        delete[] slab.addr;
    }

    if (is_attached())
        detach();
}


MemRef SlabAlloc::do_alloc(size_t size)
{
    REALM_ASSERT_DEBUG(0 < size);
    REALM_ASSERT_DEBUG((size & 0x7) == 0); // only allow sizes that are multiples of 8
    REALM_ASSERT_DEBUG(is_attached());

    // If we failed to correctly record free space, new allocations cannot be
    // carried out until the free space record is reset.
    if (m_free_space_state == free_space_Invalid)
        throw InvalidFreeSpace();
    m_free_space_state = free_space_Dirty;

    // Do we have a free space we can reuse?
    {
        typedef chunks::reverse_iterator iter;
        iter rend = m_free_space.rend();
        for (iter i = m_free_space.rbegin(); i != rend; ++i) {
            if (size <= i->size) {
                ref_type ref = i->ref;
                size_t rest = i->size - size;

                // Update free list
                if (rest == 0) {
                    // Erase by "move last over"
                    *i = m_free_space.back();
                    m_free_space.pop_back();
                }
                else {
                    i->size = rest;
                    i->ref += size;
                }

#ifdef REALM_DEBUG
                if (m_debug_out)
                    std::cerr << "Alloc ref: " << ref << " size: " << size << "\n";
#endif

                char* addr = translate(ref);
#if REALM_ENABLE_ALLOC_SET_ZERO
                std::fill(addr, addr+size, 0);
#endif
#ifdef REALM_SLAB_ALLOC_DEBUG
                malloc_debug_map[ref] = malloc(1);
#endif
                return MemRef(addr, ref);
            }
        }
    }

    // Else, allocate new slab
    size_t new_size = ((size-1) | 255) + 1; // Round up to nearest multiple of 256
    ref_type ref;
    if (m_slabs.empty()) {
        ref = m_baseline;
    }
    else {
        ref_type curr_ref_end = to_size_t(m_slabs.back().ref_end);
        // Make it at least as big as twice the previous slab
        ref_type prev_ref_end = m_slabs.size() == 1 ? m_baseline :
            to_size_t(m_slabs[m_slabs.size()-2].ref_end);
        size_t min_size = 2 * (curr_ref_end - prev_ref_end);
        if (new_size < min_size)
            new_size = min_size;
        ref = curr_ref_end;
    }
    REALM_ASSERT_DEBUG(0 < new_size);
    std::unique_ptr<char[]> mem(new char[new_size]); // Throws
    std::fill(mem.get(), mem.get()+new_size, 0);

    // Add to list of slabs
    Slab slab;
    slab.addr = mem.get();
    slab.ref_end = ref + new_size;
    m_slabs.push_back(slab); // Throws
    mem.release();

    // Update free list
    size_t unused = new_size - size;
    if (0 < unused) {
        Chunk chunk;
        chunk.ref = ref + size;
        chunk.size = unused;
        m_free_space.push_back(chunk); // Throws
    }

#ifdef REALM_DEBUG
    if (m_debug_out)
        std::cerr << "Alloc ref: " << ref << " size: " << size << "\n";
#endif

#if REALM_ENABLE_ALLOC_SET_ZERO
    std::fill(slab.addr, slab.addr+size, 0);
#endif
#ifdef REALM_SLAB_ALLOC_DEBUG
    malloc_debug_map[ref] = malloc(1);
#endif

    return MemRef(slab.addr, ref);
}


void SlabAlloc::do_free(ref_type ref, const char* addr) noexcept
{
    REALM_ASSERT_3(translate(ref), ==, addr);

    // Free space in read only segment is tracked separately
    bool read_only = is_read_only(ref);
    chunks& free_space = read_only ? m_free_read_only : m_free_space;

#ifdef REALM_SLAB_ALLOC_DEBUG
    free(malloc_debug_map[ref]);
#endif

    // Get size from segment
    size_t size = read_only ? Array::get_byte_size_from_header(addr) :
        Array::get_capacity_from_header(addr);
    ref_type ref_end = ref + size;

#ifdef REALM_DEBUG
    if (m_debug_out)
        std::cerr << "Free ref: " << ref << " size: " << size << "\n";
#endif

    if (m_free_space_state == free_space_Invalid)
        return;

    // Mutable memory cannot be freed unless it has first been allocated, and
    // any allocation puts free space tracking into the "dirty" state.
    REALM_ASSERT_3(read_only, ||, m_free_space_state == free_space_Dirty);

    m_free_space_state = free_space_Dirty;

    // Check if we can merge with adjacent succeeding free block
    typedef chunks::iterator iter;
    iter merged_with = free_space.end();
    {
        iter i = find_if(free_space.begin(), free_space.end(), ChunkRefEq(ref_end));
        if (i != free_space.end()) {
            // No consolidation over slab borders
            if (find_if(m_slabs.begin(), m_slabs.end(), SlabRefEndEq(ref_end)) == m_slabs.end()) {
                i->ref = ref;
                i->size += size;
                merged_with = i;
            }
        }
    }

    // Check if we can merge with adjacent preceeding free block (not if that
    // would cross slab boundary)
    if (find_if(m_slabs.begin(), m_slabs.end(), SlabRefEndEq(ref)) == m_slabs.end()) {
        typedef chunks::iterator iter;
        iter i = find_if(free_space.begin(), free_space.end(), ChunkRefEndEq(ref));
        if (i != free_space.end()) {
            if (merged_with != free_space.end()) {
                i->size += merged_with->size;
                // Erase by "move last over"
                *merged_with = free_space.back();
                free_space.pop_back();
            }
            else {
                i->size += size;
            }
            return;
        }
    }

    // Else just add to freelist
    if (merged_with == free_space.end()) {
        try {
            Chunk chunk;
            chunk.ref  = ref;
            chunk.size = size;
            free_space.push_back(chunk); // Throws
        }
        catch (...) {
            m_free_space_state = free_space_Invalid;
        }
    }
}


MemRef SlabAlloc::do_realloc(size_t ref, const char* addr, size_t old_size, size_t new_size)
{
    REALM_ASSERT_DEBUG(translate(ref) == addr);
    REALM_ASSERT_DEBUG(0 < new_size);
    REALM_ASSERT_DEBUG((new_size & 0x7) == 0); // only allow sizes that are multiples of 8

    // FIXME: Check if we can extend current space. In that case, remember to
    // check whether m_free_space_state == free_state_Invalid. Also remember to
    // fill with zero if REALM_ENABLE_ALLOC_SET_ZERO is non-zero.

    // Allocate new space
    MemRef new_mem = do_alloc(new_size); // Throws

    // Copy existing segment
    char* new_addr = new_mem.m_addr;
    std::copy(addr, addr+old_size, new_addr);

    // Add old segment to freelist
    do_free(ref, addr);

#ifdef REALM_DEBUG
    if (m_debug_out) {
        std::cerr << "Realloc orig_ref: " << ref << " old_size: " << old_size << " "
            "new_ref: " << new_mem.m_ref << " new_size: " << new_size << "\n";
    }
#endif // REALM_DEBUG

    return new_mem;
}


char* SlabAlloc::do_translate(ref_type ref) const noexcept
{
    REALM_ASSERT_DEBUG(is_attached());

    char* addr = nullptr;

    size_t cache_index = ref ^ ((ref >> 16) >> 16);
    // we shift by 16 two times. On 32-bitters it's undefined to shift by
    // 32. Shifting twice x16 however, is defined and gives zero. On 64-bitters
    // the compiler should reduce it to a single 32 bit shift.
    cache_index = cache_index ^(cache_index >> 16);
    cache_index = (cache_index ^(cache_index >> 8)) & 0xFF;
    if (cache[cache_index].ref == ref && cache[cache_index].version == version)
        return cache[cache_index].addr;

    if (ref < m_baseline) {

        const util::File::Map<char>* map;

        // fast path if reference is inside the initial mapping (or buffer):
        if (ref < m_initial_chunk_size) {
            addr = m_data + ref;
            if (m_file_mappings) {
                // Once established, the initial mapping is immutable, so we
                // don't need to grab a lock for access.
                map = &m_file_mappings->m_initial_mapping;
                realm::util::encryption_read_barrier(addr, Array::header_size,
                                                     map->get_encrypted_mapping(),
                                                     Array::get_byte_size_from_header);
            }
        }
        else {
            // reference must be inside a section mapped later
            size_t section_index = get_section_index(ref);
            REALM_ASSERT_DEBUG(m_file_mappings);

            // Access to the additional mappings may conflict with allocation of a new
            // array of mappings (or more precisely: it may conflict with the deletion
            // of the array as it is being replaced with a new one), so we must lock it
            std::lock_guard<util::Mutex> lock(m_file_mappings->m_mutex);

            size_t mapping_index = section_index - m_file_mappings->m_first_additional_mapping;
            size_t section_offset = ref - get_section_base(section_index);
            REALM_ASSERT_DEBUG(m_file_mappings->m_additional_mappings);
            REALM_ASSERT_DEBUG(mapping_index < m_file_mappings->m_num_additional_mappings);
            map = &m_file_mappings->m_additional_mappings[mapping_index];
            REALM_ASSERT_DEBUG(map->get_addr() != nullptr);
            addr = map->get_addr() + section_offset;
            realm::util::encryption_read_barrier(addr, Array::header_size,
                                                 map->get_encrypted_mapping(),
                                                 Array::get_byte_size_from_header);
        }
    }
    else {
        typedef slabs::const_iterator iter;
        iter i = upper_bound(m_slabs.begin(), m_slabs.end(), ref, &ref_less_than_slab_ref_end);
        REALM_ASSERT_DEBUG(i != m_slabs.end());

        ref_type slab_ref = i == m_slabs.begin() ? m_baseline : (i-1)->ref_end;
        addr = i->addr + (ref - slab_ref);
    }
    cache[cache_index].addr = addr;
    cache[cache_index].ref = ref;
    cache[cache_index].version = version;
    REALM_ASSERT_DEBUG(addr != nullptr);
    return addr;
}


int SlabAlloc::get_committed_file_format_version() const noexcept
{
    Header& header = *reinterpret_cast<Header*>(m_data);
    int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
    int file_format_version = int(header.m_file_format[slot_selector]);
    return file_format_version;
}


std::map<std::string, std::weak_ptr<SlabAlloc::MappedFile>> all_files;
util::Mutex all_files_mutex;

ref_type SlabAlloc::attach_file(const std::string& path, Config& cfg)
{
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT(!is_attached());

    // When 'read_only' is true, this function will throw InvalidDatabase if the
    // file exists already but is empty. This can happen if another process is
    // currently creating it. Note however, that it is only legal for multiple
    // processes to access a database file concurrently if it is done via a
    // SharedGroup, and in that case 'read_only' can never be true.
    REALM_ASSERT(!(cfg.is_shared && cfg.read_only));
    // session_initiator can be set *only* if we're shared.
    REALM_ASSERT(cfg.is_shared || !cfg.session_initiator);
    // clear_file can be set *only* if we're the first session.
    REALM_ASSERT(cfg.session_initiator || !cfg.clear_file);

    using namespace realm::util;
    File::AccessMode access = cfg.read_only ? File::access_ReadOnly : File::access_ReadWrite;
    File::CreateMode create = cfg.read_only || cfg.no_create ? File::create_Never : File::create_Auto;
    {
        std::lock_guard<Mutex> lock(all_files_mutex);
        try {
            std::shared_ptr<SlabAlloc::MappedFile> p(all_files[path]);
            m_file_mappings = p;
        }
        catch (...) {
            m_file_mappings = std::make_shared<MappedFile>();
            all_files[path] = m_file_mappings;
        }
    }
    std::lock_guard<Mutex> lock(m_file_mappings->m_mutex);
    if (m_file_mappings->m_success) {
        REALM_ASSERT(!cfg.session_initiator);
        m_file_format_version = m_file_mappings->m_file_format_version;
        m_data = m_file_mappings->m_initial_mapping.get_addr();
        m_initial_chunk_size = m_file_mappings->m_initial_mapping.get_size();
        m_attach_mode = cfg.is_shared ? attach_SharedFile : attach_UnsharedFile;
        m_free_space_state = free_space_Invalid;
        if (m_file_mappings->m_num_additional_mappings) {
            int mapping_index = m_file_mappings->m_num_additional_mappings;
            int section_index = mapping_index + m_file_mappings->m_first_additional_mapping;
            m_baseline = get_section_base(section_index);
        }
        else {
            m_baseline = m_file_mappings->m_initial_mapping.get_size();
        }
        return 0;
    }

    m_file_mappings->m_file.open(path.c_str(), access, create, 0); // Throws
    if (cfg.encryption_key)
        m_file_mappings->m_file.set_encryption_key(cfg.encryption_key);
    File::CloseGuard fcg(m_file_mappings->m_file);

    size_t initial_size_of_file;
    size_t size;

    // We can only safely mmap the file, if its size matches a section. If not,
    // we must change the size to match before mmaping it.
    // This can fail due to a race with a concurrent commmit, in which case we
    // must throw allowing the caller to retry, but the common case is to succeed
    // at first attempt

    // The size of a database file must not exceed what can be encoded in
    // size_t.
    if (REALM_UNLIKELY(int_cast_with_overflow_detect(m_file_mappings->m_file.get_size(), size)))
        throw InvalidDatabase("Realm file too large", path);

    // FIXME: This initialization procedure does not provide sufficient
    // robustness given that processes may be abruptly terminated at any point
    // in time. In unshared mode, we must be able to reliably detect any invalid
    // file as long as its invalidity is due to a terminated serialization
    // process (e.g. due to a power failure). In shared mode we can guarantee
    // that if the database file was ever valid, then it will remain valid,
    // however, there is no way we can ensure that initialization of an empty
    // database file succeeds. Thus, in shared mode we must be able to reliably
    // distiguish between three cases when opening a database file: A) It was
    // never properly initialized. In this case we should simply reinitialize
    // it. B) It looks corrupt. In this case we throw an exception. C) It looks
    // good. In this case we proceede as normal.
    if (size == 0 || cfg.clear_file) {
        if (REALM_UNLIKELY(cfg.read_only))
            throw InvalidDatabase("Read-only access to empty Realm file", path);

        const char* data = reinterpret_cast<const char*>(&empty_file_header);
        m_file_mappings->m_file.write(data, sizeof empty_file_header); // Throws

        // Pre-alloc initial space
        size_t initial_size = m_initial_section_size;
        m_file_mappings->m_file.prealloc(0, initial_size); // Throws
        bool disable_sync = get_disable_sync_to_disk();
        if (!disable_sync)
            m_file_mappings->m_file.sync(); // Throws
        size = initial_size;
    }

    // We must now make sure the filesize matches a mmap boundary...
    // first, save the original filesize for use during validation and
    // (potentially) conversion from streaming format.
    initial_size_of_file = size;

    if (!matches_section_boundary(size)) {
        // The file size did not match a section boundary.
        // We must extend the file to a section boundary (unless already there)
        // The file must be extended to match in size prior to being mmapped,
        // as extending it after mmap has undefined behavior.

        // The mapping of the first part of the file *must* be contiguous, because
        // we do not know if the file was created by a version of the code, that took
        // the section boundaries into account. If it wasn't we cannot map it in sections
        // without risking datastructures that cross a mapping boundary.

        if (cfg.read_only) {

            // If the file is opened read-only, we cannot extend it. This is not a problem,
            // because for a read-only file we assume that it will not change while we use it.
            // This assumption obviously will not hold, if the file is shared by multiple
            // processes or threads with different opening modes.
            // Currently, there is no way to detect if this assumption is violated.
            ;
        }
        else {

            if (cfg.session_initiator || !cfg.is_shared) {

                // We can only safely extend the file if we're the session initiator, or if
                // the file isn't shared at all.

                // resizing the file (as we do here) without actually changing any internal
                // datastructures to reflect the additional free space will work, because the
                // free space management relies on the logical filesize and disregards the
                // actual size of the file.
                size = get_upper_section_boundary(size);
                m_file_mappings->m_file.prealloc(0, size);
            }
            else {
                // Getting here, we have a file of a size that will not work, and without being
                // allowed to extend it.
                // This can happen in the case where a concurrent commit is extending the file,
                // and we observe it part-way (file extension is not atomic). If so, we
                // need to start all over. The alternative would be to synchronize with commit,
                // and we generally try to avoid this when possible.
                throw Retry();
            }
        }
    }

    ref_type top_ref;
    try {
        File::Map<char> map(m_file_mappings->m_file, File::access_ReadOnly, size); // Throws
        // we'll read header and (potentially) footer
        realm::util::encryption_read_barrier(map, 0, sizeof(Header));
        realm::util::encryption_read_barrier(map, initial_size_of_file - sizeof(Header), sizeof(Header));

        if (!cfg.skip_validate) {
            // Verify the data structures
            validate_buffer(map.get_addr(), initial_size_of_file, path, cfg.is_shared); // Throws
        }

        {
            const Header& header = reinterpret_cast<const Header&>(*map.get_addr());
            int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
            m_file_mappings->m_file_format_version = header.m_file_format[slot_selector];
            uint_fast64_t ref = uint_fast64_t(header.m_top_ref[slot_selector]);
            m_file_on_streaming_form = (slot_selector == 0 && ref == 0xFFFFFFFFFFFFFFFFULL);
            m_file_mappings->m_file_on_streaming_form = m_file_on_streaming_form;
            if (m_file_on_streaming_form) {
                const StreamingFooter& footer =
                    *(reinterpret_cast<StreamingFooter*>(map.get_addr()+initial_size_of_file) - 1);
                top_ref = ref_type(footer.m_top_ref);
            }
            else {
                top_ref = ref_type(ref);
            }
        }

        m_data = map.get_addr();
        m_file_mappings->m_initial_mapping = std::move(map);
        m_baseline = size;
        m_initial_chunk_size = size;
        m_file_mappings->m_first_additional_mapping = get_section_index(m_initial_chunk_size);
        m_attach_mode = cfg.is_shared ? attach_SharedFile : attach_UnsharedFile;

        // Below this point (assignment to `m_attach_mode`), nothing must throw.
    }
    catch (DecryptionFailed) {
        throw InvalidDatabase("Realm file decryption failed", path);
    }

    // make sure that any call to begin_read cause any slab to be placed in free
    // lists correctly
    m_free_space_state = free_space_Invalid;

    // make sure the database is not on streaming format. This has to be done at
    // session initialization, even if it means writing the database during open.
    //
    // FIXME: Why does this need to be done? Explanation needed.
    if (cfg.session_initiator && m_file_mappings->m_file_on_streaming_form) {
        const Header& header = *reinterpret_cast<Header*>(m_data);
        const StreamingFooter& footer =
            *(reinterpret_cast<StreamingFooter*>(m_data+initial_size_of_file) - 1);
        // Don't compare file format version fields as they are allowed to differ.
        // Also don't compare reserved fields (todo, is it correct to ignore?)
        static_cast<void>(header);
        REALM_ASSERT_3(header.m_flags, == , 0);
        REALM_ASSERT_3(header.m_mnemonic[0], == , uint8_t('T'));
        REALM_ASSERT_3(header.m_mnemonic[1], == , uint8_t('-'));
        REALM_ASSERT_3(header.m_mnemonic[2], == , uint8_t('D'));
        REALM_ASSERT_3(header.m_mnemonic[3], == , uint8_t('B'));
        REALM_ASSERT_3(header.m_top_ref[0], == , 0xFFFFFFFFFFFFFFFFULL);
        REALM_ASSERT_3(header.m_top_ref[1], == , 0);

        REALM_ASSERT_3(footer.m_magic_cookie, ==, footer_magic_cookie);
        {
            File::Map<Header> writable_map(m_file_mappings->m_file, File::access_ReadWrite,
                                           sizeof (Header)); // Throws
            Header& writable_header = *writable_map.get_addr();
            realm::util::encryption_read_barrier(writable_map, 0);
            writable_header.m_top_ref[1] = footer.m_top_ref;
            writable_header.m_file_format[1] = writable_header.m_file_format[0];
            realm::util::encryption_write_barrier(writable_map, 0);
            writable_map.sync();
            realm::util::encryption_read_barrier(writable_map, 0);
            writable_header.m_flags |= flags_SelectBit;
            realm::util::encryption_write_barrier(writable_map, 0);
            m_file_on_streaming_form = false;
            m_file_mappings->m_file_on_streaming_form = false;
            writable_map.sync();
        }
    }

    fcg.release(); // Do not close
    m_file_mappings->m_success = true;
    return top_ref;
}

ref_type SlabAlloc::attach_buffer(char* data, size_t size)
{
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT(!is_attached());

    // Verify the data structures
    std::string path; // No path
    bool is_shared = false;
    validate_buffer(data, size, path, is_shared); // Throws

    ref_type top_ref;
    {
        const Header& header = reinterpret_cast<const Header&>(*data);
        int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
        m_file_format_version = header.m_file_format[slot_selector];
        uint_fast64_t ref = uint_fast64_t(header.m_top_ref[slot_selector]);
        m_file_on_streaming_form = (slot_selector == 0 && ref == 0xFFFFFFFFFFFFFFFFULL);
        if (m_file_on_streaming_form) {
            const StreamingFooter& footer = *(reinterpret_cast<StreamingFooter*>(data+size) - 1);
            top_ref = ref_type(footer.m_top_ref);
        }
        else {
            top_ref = ref_type(ref);
        }
    }

    m_data        = data;
    m_baseline    = size;
    m_initial_chunk_size = size;
    m_attach_mode = attach_UsersBuffer;

    // Below this point (assignment to `m_attach_mode`), nothing must throw.

    return top_ref;
}


void SlabAlloc::attach_empty()
{
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT(!is_attached());

    m_file_format_version = 0; // Not yet decided
    m_attach_mode = attach_OwnedBuffer;
    m_data = nullptr; // Empty buffer

    // Below this point (assignment to `m_attach_mode`), nothing must throw.

    // No ref must ever be less that the header size, so we will use that as the
    // baseline here.
    m_baseline = sizeof (Header);
    m_initial_chunk_size = m_baseline;
}


void SlabAlloc::validate_buffer(const char* data, size_t size, const std::string& path,
                                bool is_shared)
{
    // Verify that size is sane and 8-byte aligned
    if (REALM_UNLIKELY(size < sizeof (Header) || size % 8 != 0))
        throw InvalidDatabase("Realm file has bad size", path);

    const Header& header = *reinterpret_cast<const Header*>(data);

    // First four bytes of info block is file format id
    if (REALM_UNLIKELY(!(char(header.m_mnemonic[0]) == 'T' &&
                         char(header.m_mnemonic[1]) == '-' &&
                         char(header.m_mnemonic[2]) == 'D' &&
                         char(header.m_mnemonic[3]) == 'B')))
        throw InvalidDatabase("Not a Realm file", path);

    // Last bit in info block indicates which top_ref block is valid
    int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);

    // Top-ref must always point within buffer
    uint_fast64_t top_ref = uint_fast64_t(header.m_top_ref[slot_selector]);
    if (slot_selector == 0 && top_ref == 0xFFFFFFFFFFFFFFFFULL) {
        if (REALM_UNLIKELY(size < sizeof (Header) + sizeof (StreamingFooter)))
            throw InvalidDatabase("Realm file in streaming form has bad size", path);
        const StreamingFooter& footer = *(reinterpret_cast<const StreamingFooter*>(data+size) - 1);
        top_ref = footer.m_top_ref;
        if (REALM_UNLIKELY(footer.m_magic_cookie != footer_magic_cookie))
            throw InvalidDatabase("Bad Realm file header (#1)", path);
    }
    if (REALM_UNLIKELY(top_ref % 8 != 0))
        throw InvalidDatabase("Bad Realm file header (#2)", path);
    if (REALM_UNLIKELY(top_ref >= size))
        throw InvalidDatabase("Bad Realm file header (#3)", path);

    // Check file format version. For information about the differences between
    // particular file format versions, refer to the documentation for
    // get_file_format_version().
    bool bad_file_format = true;
    int file_format_version = int(header.m_file_format[slot_selector]);
    if (file_format_version == 0) { // Not yet decided
        if (top_ref == 0)
            bad_file_format = false;
    }
    else if (is_shared) {
        // In shared mode (Realm file opened via a SharedGroup instance) this
        // version of the core library is able to open Realms using file format
        // versions 2, 3, and 4. Versoin 2 files always need to be
        // upgraded. Version 3 files only need to be upgraded when using an
        // in-Realm history (see Replication::get_history_type()).
        switch (file_format_version) {
            case 2:
            case 3:
            case 4:
                bad_file_format = false;
        }
    }
    else {
        // In non-shared mode (Realm file opened via a Group instance) this
        // version of the core library is able to open Realms using file format
        // versions 3 and 4. Since a Realm file cannot be upgraded when opened
        // in this mode (we may be unable to write to the file), versoin 2 files
        // cannot be opened. In-Realm histories require file format 4, but this
        // is not a problem, as no history is used non-shared mode.
        switch (file_format_version) {
            case 3:
            case 4:
                bad_file_format = false;
        }
    }
    if (REALM_UNLIKELY(bad_file_format))
        throw InvalidDatabase("Unsupported Realm file format version", path);
}


size_t SlabAlloc::get_total_size() const noexcept
{
    return m_slabs.empty() ? m_baseline : m_slabs.back().ref_end;
}


void SlabAlloc::reset_free_space_tracking()
{
    invalidate_cache();
    if (is_free_space_clean())
        return;

    // Free all scratch space (done after all data has
    // been commited to persistent space)
    m_free_read_only.clear();
    m_free_space.clear();

    // Rebuild free list to include all slabs
    Chunk chunk;
    chunk.ref = m_baseline;

    for (const auto& slab : m_slabs) {
        chunk.size = slab.ref_end - chunk.ref;
        m_free_space.push_back(chunk); // Throws
        chunk.ref = slab.ref_end;
    }

    REALM_ASSERT_DEBUG(is_all_free());

    m_free_space_state = free_space_Clean;
}


void SlabAlloc::remap(size_t file_size)
{
    REALM_ASSERT_DEBUG(file_size % 8 == 0); // 8-byte alignment required
    REALM_ASSERT_DEBUG(m_attach_mode == attach_SharedFile || m_attach_mode == attach_UnsharedFile);
    REALM_ASSERT_DEBUG(is_free_space_clean());
    REALM_ASSERT_DEBUG(m_baseline <= file_size);

    // Extend mapping by adding sections
    REALM_ASSERT_DEBUG(matches_section_boundary(file_size));
    m_baseline = file_size;
    auto num_sections = get_section_index(file_size);
    {
        std::lock_guard<util::Mutex> lock(m_file_mappings->m_mutex);
        auto num_additional_mappings = num_sections - m_file_mappings->m_first_additional_mapping;

        if (num_additional_mappings > m_file_mappings->m_capacity_additional_mappings) {
            // FIXME: No harcoded constants here
            m_file_mappings->m_capacity_additional_mappings = num_additional_mappings + 128;
            std::unique_ptr<util::File::Map<char>[]> new_mappings;
            new_mappings.reset(new util::File::Map<char>[m_file_mappings->m_capacity_additional_mappings]);
            for (size_t j = 0; j < m_file_mappings->m_num_additional_mappings; ++j)
                new_mappings[j] = std::move(m_file_mappings->m_additional_mappings[j]);
            m_file_mappings->m_additional_mappings = std::move(new_mappings);
        }
        for (size_t k = m_file_mappings->m_num_additional_mappings; k < num_additional_mappings; ++k)
        {
            auto section_start_offset = get_section_base(k + m_file_mappings->m_first_additional_mapping);
            auto section_size = get_section_base(1 + k + m_file_mappings->m_first_additional_mapping) - section_start_offset;
            util::File::Map<char> map(m_file_mappings->m_file, section_start_offset, File::access_ReadOnly, section_size);
            m_file_mappings->m_additional_mappings[k] = std::move(map);
        }
        m_file_mappings->m_num_additional_mappings = num_additional_mappings;
    }
    // Rebase slabs and free list (assumes exactly one entry in m_free_space for
    // each entire slab in m_slabs)
    size_t slab_ref = file_size;
    size_t n = m_free_space.size();
    REALM_ASSERT_DEBUG(m_slabs.size() == n);
    for (size_t i = 0; i < n; ++i) {
        Chunk& free_chunk = m_free_space[i];
        free_chunk.ref = slab_ref;
        ref_type slab_ref_end = slab_ref + free_chunk.size;
        m_slabs[i].ref_end = slab_ref_end;
        slab_ref = slab_ref_end;
    }
}

const SlabAlloc::chunks& SlabAlloc::get_free_read_only() const
{
    if (m_free_space_state == free_space_Invalid)
        throw InvalidFreeSpace();
    return m_free_read_only;
}



// A database file is viewed as a number of sections of exponentially growing size.
// The first 16 sections are 1 x page size, the next 8 sections are 2 x page size,
// then follows 8 sections of 4 x page size, 8 sections of 8 x page size and so forth.
// This layout makes it possible to determine the section number for a given offset
// into the file in constant time using a bit scan intrinsic and a few bit manipulations.
// The get_section_index() method determines the section number from the offset, while
// the get_section_base() does the opposite, giving the starting offset for a given
// section number.
//
// Please note that the file is not necessarily mmapped with a separate mapping
// for each section, multiple sections may be mmapped with a single mmap.

size_t SlabAlloc::get_section_index(size_t pos) const noexcept
{
    // size_t section_base_number = pos/m_initial_section_size;
    size_t section_base_number = pos >> m_section_shifts;
    size_t section_group_number = section_base_number/16;
    size_t index;
    if (section_group_number == 0) {
        // first 16 entries aligns 1:1
        index = section_base_number;
    }
    else {
        // remaning entries are exponential
        size_t log_index = log2(section_group_number);
        size_t section_index_in_group = (section_base_number >> (1+log_index)) & 0x7;
        index = (16 + (log_index * 8)) + section_index_in_group;
    }
    return index;
}

size_t SlabAlloc::compute_section_base(size_t index) const noexcept
{
    size_t base;
    if (index < 16) {
        // base = index * m_initial_section_size;
        base = index << m_section_shifts;
    }
    else {
        size_t section_index_in_group = index & 7;
        size_t log_index = (index - section_index_in_group)/8 - 2;
        size_t section_base_number = (8 + section_index_in_group)<<(1+log_index);
        // base = m_initial_section_size * section_base_number;
        base = section_base_number << m_section_shifts;
    }
    return base;
}

size_t SlabAlloc::find_section_in_range(size_t start_pos,
                                             size_t free_chunk_size,
                                             size_t request_size) const noexcept
{
    size_t end_of_block = start_pos + free_chunk_size;
    size_t alloc_pos = start_pos;
    while (alloc_pos + request_size <= end_of_block) {
        size_t next_section_boundary = get_upper_section_boundary(alloc_pos);
        if (alloc_pos + request_size <= next_section_boundary) {
            return alloc_pos;
        }
        alloc_pos = next_section_boundary;
    }
    return 0;
}


void SlabAlloc::resize_file(size_t new_file_size)
{
    std::lock_guard<Mutex> lock(m_file_mappings->m_mutex);
    m_file_mappings->m_file.prealloc(0, new_file_size); // Throws
    bool disable_sync = get_disable_sync_to_disk();
    if (!disable_sync)
        m_file_mappings->m_file.sync(); // Throws
}

void SlabAlloc::reserve_disk_space(size_t size)
{
    std::lock_guard<Mutex> lock(m_file_mappings->m_mutex);
    m_file_mappings->m_file.prealloc_if_supported(0, size); // Throws
    bool disable_sync = get_disable_sync_to_disk();
    if (!disable_sync)
        m_file_mappings->m_file.sync(); // Throws
}

void SlabAlloc::set_file_format_version(int file_format_version) noexcept
{
    m_file_format_version = file_format_version;
    // FIXME does this invalidate the ability to share the mapping??
}




#ifdef REALM_DEBUG

bool SlabAlloc::is_all_free() const
{
    if (m_free_space.size() != m_slabs.size())
        return false;

    // Verify that free space matches slabs
    ref_type slab_ref = m_baseline;
    for (const auto& slab : m_slabs) {
        size_t slab_size = slab.ref_end - slab_ref;
        chunks::const_iterator chunk =
            find_if(m_free_space.begin(), m_free_space.end(), ChunkRefEq(slab_ref));
        if (chunk == m_free_space.end())
            return false;
        if (slab_size != chunk->size)
            return false;
        slab_ref = slab.ref_end;
    }
    return true;
}


void SlabAlloc::verify() const
{
    // Make sure that all free blocks fit within a slab
    for (const auto& chunk : m_free_space) {
        slabs::const_iterator slab =
            upper_bound(m_slabs.begin(), m_slabs.end(), chunk.ref, &ref_less_than_slab_ref_end);
        REALM_ASSERT(slab != m_slabs.end());

        ref_type slab_ref_end = slab->ref_end;
        ref_type chunk_ref_end = chunk.ref + chunk.size;
        REALM_ASSERT_3(chunk_ref_end, <=, slab_ref_end);
    }
}


void SlabAlloc::print() const
{
    size_t allocated_for_slabs = m_slabs.empty() ? 0 : m_slabs.back().ref_end - m_baseline;

    size_t free = 0;
    for (const auto& free_block : m_free_space) {
        free += free_block.size;
    }

    size_t allocated = allocated_for_slabs - free;
    std::cout << "Attached: " << (m_data ? m_baseline : 0) << " Allocated: " << allocated << "\n";

    if (!m_slabs.empty()) {
        std::cout << "Slabs: ";
        ref_type first_ref = m_baseline;

        for (const auto& slab : m_slabs) {
            if (&slab != &m_slabs.front())
                std::cout << ", ";

            ref_type last_ref = slab.ref_end - 1;
            size_t size = slab.ref_end - first_ref;
            void* addr = slab.addr;
            std::cout << "(" << first_ref << "->" << last_ref << ", size=" << size << ", addr=" << addr << ")";
            first_ref = slab.ref_end;
        }
        std::cout << "\n";
    }

    if (!m_free_space.empty()) {
        std::cout << "FreeSpace: ";
        for (const auto& free_block : m_free_space) {
            if (&free_block != &m_free_space.front())
                std::cout << ", ";

            ref_type last_ref = free_block.ref + free_block.size - 1;
            std::cout << "(" << free_block.ref << "->" << last_ref << ", size=" << free_block.size << ")";
        }
        std::cout << "\n";
    }
    if (!m_free_read_only.empty()) {
        std::cout << "FreeSpace (ro): ";
        for (const auto& free_block : m_free_read_only) {
            if (&free_block != &m_free_read_only.front())
                std::cout << ", ";

            ref_type last_ref = free_block.ref + free_block.size - 1;
            std::cout << "(" << free_block.ref << "->" << last_ref << ", size=" << free_block.size << ")";
        }
        std::cout << "\n";
    }
    std::cout << std::flush;
}

#endif // REALM_DEBUG
