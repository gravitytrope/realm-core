#ifdef _MSC_VER
#  include <win32/types.h>
#endif

#include <tightdb/array_binary.hpp>
#include <tightdb/array_blob.hpp>

using namespace std;
using namespace tightdb;


ArrayBinary::ArrayBinary(ArrayParent* parent, size_t ndx_in_parent, Allocator& alloc):
    Array(type_HasRefs, parent, ndx_in_parent, alloc),
    m_offsets(type_Normal, 0, 0, alloc), m_blob(0, 0, alloc)
{
    // Add subarrays for long string
    Array::add(m_offsets.get_ref());
    Array::add(m_blob.get_ref());
    m_offsets.set_parent(this, 0);
    m_blob.set_parent(this, 1);
}

ArrayBinary::ArrayBinary(MemRef mem, ArrayParent* parent, size_t ndx_in_parent,
                         Allocator& alloc) TIGHTDB_NOEXCEPT:
    Array(mem, parent, ndx_in_parent, alloc), m_offsets(Array::get_as_ref(0), 0, 0, alloc),
    m_blob(Array::get_as_ref(1), 0, 0, alloc)
{
    // has_refs() indicates that this is a long string
    TIGHTDB_ASSERT(has_refs() && !is_inner_bptree_node());
    TIGHTDB_ASSERT(Array::size() == 2);
    TIGHTDB_ASSERT(m_blob.size() == (m_offsets.is_empty() ? 0 : to_size_t(m_offsets.back())));

    m_offsets.set_parent(this, 0);
    m_blob.set_parent(this, 1);
}

ArrayBinary::ArrayBinary(ref_type ref, ArrayParent* parent, size_t ndx_in_parent,
                         Allocator& alloc) TIGHTDB_NOEXCEPT:
    Array(ref, parent, ndx_in_parent, alloc), m_offsets(Array::get_as_ref(0), 0, 0, alloc),
    m_blob(Array::get_as_ref(1), 0, 0, alloc)
{
    // has_refs() indicates that this is a long string
    TIGHTDB_ASSERT(has_refs() && !is_inner_bptree_node());
    TIGHTDB_ASSERT(Array::size() == 2);
    TIGHTDB_ASSERT(m_blob.size() == (m_offsets.is_empty() ? 0 : to_size_t(m_offsets.back())));

    m_offsets.set_parent(this, 0);
    m_blob.set_parent(this, 1);
}

void ArrayBinary::add(BinaryData value, bool add_zero_term)
{
    TIGHTDB_ASSERT(value.size() == 0 || value.data());

    m_blob.add(value.data(), value.size(), add_zero_term);
    size_t stored_size = value.size();
    if (add_zero_term)
        ++stored_size;
    size_t offset = stored_size;
    if (!m_offsets.is_empty())
        offset += m_offsets.back();//fixme:32bit:src\tightdb\array_binary.cpp(61): warning C4244: '+=' : conversion from 'int64_t' to 'size_t', possible loss of data
    m_offsets.add(offset);
}

void ArrayBinary::set(size_t ndx, BinaryData value, bool add_zero_term)
{
    TIGHTDB_ASSERT(ndx < m_offsets.size());
    TIGHTDB_ASSERT(value.size() == 0 || value.data());

    size_t start = ndx ? to_size_t(m_offsets.get(ndx-1)) : 0;
    size_t current_end = to_size_t(m_offsets.get(ndx));
    size_t stored_size = value.size();
    if (add_zero_term)
        ++stored_size;
    ssize_t diff =  (start + stored_size) - current_end;
    m_blob.replace(start, current_end, value.data(), value.size(), add_zero_term);
    m_offsets.adjust(ndx, m_offsets.size(), diff);
}

void ArrayBinary::insert(size_t ndx, BinaryData value, bool add_zero_term)
{
    TIGHTDB_ASSERT(ndx <= m_offsets.size());
    TIGHTDB_ASSERT(value.size() == 0 || value.data());

    size_t pos = ndx ? to_size_t(m_offsets.get(ndx-1)) : 0;
    m_blob.insert(pos, value.data(), value.size(), add_zero_term);

    size_t stored_size = value.size();
    if (add_zero_term)
        ++stored_size;
    m_offsets.insert(ndx, pos + stored_size);
    m_offsets.adjust(ndx+1, m_offsets.size(), stored_size);
}

void ArrayBinary::erase(size_t ndx)
{
    TIGHTDB_ASSERT(ndx < m_offsets.size());

    size_t start = ndx ? to_size_t(m_offsets.get(ndx-1)) : 0;
    size_t end = to_size_t(m_offsets.get(ndx));

    m_blob.erase(start, end);
    m_offsets.erase(ndx);
    m_offsets.adjust(ndx, m_offsets.size(), int64_t(start) - end);
}

BinaryData ArrayBinary::get(const char* header, size_t ndx, Allocator& alloc) TIGHTDB_NOEXCEPT
{
    pair<int_least64_t, int_least64_t> p = get_two(header, 0);
    const char* offsets_header = alloc.translate(to_ref(p.first));
    const char* blob_header = alloc.translate(to_ref(p.second));
    size_t begin, end;
    if (ndx) {
        p = get_two(offsets_header, ndx-1);
        begin = to_size_t(p.first);
        end   = to_size_t(p.second);
    }
    else {
        begin = 0;
        end   = to_size_t(Array::get(offsets_header, ndx));
    }
    return BinaryData(ArrayBlob::get(blob_header, begin), end-begin);
}

ref_type ArrayBinary::bptree_leaf_insert(size_t ndx, BinaryData value, bool add_zero_term,
                                         TreeInsertBase& state)
{
    size_t leaf_size = size();
    TIGHTDB_ASSERT(leaf_size <= TIGHTDB_MAX_LIST_SIZE);
    if (leaf_size < ndx)
        ndx = leaf_size;
    if (TIGHTDB_LIKELY(leaf_size < TIGHTDB_MAX_LIST_SIZE)) {
        insert(ndx, value, add_zero_term);
        return 0; // Leaf was not split
    }

    // Split leaf node
    ArrayBinary new_leaf(0, 0, get_alloc());
    if (ndx == leaf_size) {
        new_leaf.add(value, add_zero_term);
        state.m_split_offset = ndx;
    }
    else {
        for (size_t i = ndx; i != leaf_size; ++i)
            new_leaf.add(get(i));
        truncate(ndx);
        add(value, add_zero_term);
        state.m_split_offset = ndx + 1;
    }
    state.m_split_size = leaf_size + 1;
    return new_leaf.get_ref();
}


ref_type ArrayBinary::create_array(std::size_t size, Allocator& alloc)
{
    Array top(alloc);
    top.create(type_HasRefs); // Throws
    try {
        int_fast64_t value = 0;
        ref_type offsets_ref = Array::create_array(type_Normal, size, value, alloc); // Throws
        try {
            int_fast64_t v = offsets_ref; // FIXME: Dangerous cast: unsigned -> signed
            top.add(v); // Throws
        }
        catch (...) {
            Array::destroy(offsets_ref, alloc);
            throw;
        }
        size_t blobs_size = 0;
        ref_type blobs_ref = ArrayBlob::create_array(blobs_size, alloc); // Throws
        try {
            int_fast64_t v = blobs_ref; // FIXME: Dangerous cast: unsigned -> signed
            top.add(v); // Throws
        }
        catch (...) {
            Array::destroy(blobs_ref, alloc);
            throw;
        }
        return top.get_ref();
    }
    catch (...) {
        top.destroy_deep();
        throw;
    }
}


#ifdef TIGHTDB_DEBUG

void ArrayBinary::to_dot(ostream& out, bool, StringData title) const
{
    ref_type ref = get_ref();

    out << "subgraph cluster_binary" << ref << " {" << endl;
    out << " label = \"ArrayBinary";
    if (title.size() != 0)
        out << "\\n'" << title << "'";
    out << "\";" << endl;

    Array::to_dot(out, "binary_top");
    m_offsets.to_dot(out, "offsets");
    m_blob.to_dot(out, "blob");

    out << "}" << endl;
}

#endif // TIGHTDB_DEBUG
