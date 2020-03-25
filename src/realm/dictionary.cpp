/*************************************************************************
 *
 * Copyright 2019 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/dictionary.hpp>
#include <algorithm>

namespace realm {

namespace {

struct MixedIterator {
    typedef std::random_access_iterator_tag iterator_category;
    typedef const Mixed value_type;
    typedef ptrdiff_t difference_type;
    typedef const Mixed* pointer;
    typedef const Mixed& reference;

    MixedIterator(BPlusTree<Mixed>* arr, size_t n)
        : m_arr(arr)
        , m_pos(n)
    {
    }
    size_t get_pos()
    {
        return m_pos;
    }
    Mixed operator*() const
    {
        return m_arr->get(m_pos);
    }
    MixedIterator& operator++()
    {
        m_pos++;
        return *this;
    }
    MixedIterator& operator+=(size_t i)
    {
        m_pos += i;
        return *this;
    }
    MixedIterator& operator--()
    {
        m_pos--;
        return *this;
    }
    ptrdiff_t operator-(const MixedIterator& other)
    {
        return m_pos - other.m_pos;
    }
    bool operator!=(const MixedIterator& rhs)
    {
        return m_pos != rhs.m_pos;
    }

    BPlusTree<Mixed>* m_arr;
    size_t m_pos;
};

std::pair<size_t, bool> lower_bound_mixed(BPlusTree<Mixed>* arr, Mixed value)
{
    auto sz = arr->size();
    MixedIterator begin(arr, 0);
    MixedIterator end(arr, sz);
    auto p = std::lower_bound(begin, end, value).m_pos;
    if (p < sz && arr->get(p) == value) {
        return {p, true};
    }
    return {p, false};
}

} // namespace

/******************************** Dictionary *********************************/

Dictionary& Dictionary::operator=(const Dictionary& other)
{
    if (this != &other) {
        m_obj = other.m_obj;
        m_valid = other.m_valid;
        m_col_key = other.m_col_key;
        m_keys = nullptr;
        m_values = nullptr;

        if (other.m_keys) {
            Allocator& alloc = other.m_keys->get_alloc();
            m_keys = std::make_unique<BPlusTree<Mixed>>(alloc);
            m_values = std::make_unique<BPlusTree<Mixed>>(alloc);
            size_t ndx = m_obj.get_row_ndx();
            m_keys->set_parent(this, ndx * 2);
            m_values->set_parent(this, ndx * 2 + 1);
            init_from_parent();
        }
    }
    return *this;
}

Mixed Dictionary::get(Mixed key) const
{
    update_if_needed();
    if (m_valid) {
        auto match = lower_bound_mixed(m_keys.get(), key);

        if (match.second) {
            return m_values->get(match.first);
        }
    }
    throw std::out_of_range("Key not found");
    return {};
}

Dictionary::Iterator Dictionary::begin() const
{
    return Iterator(this, 0);
}

Dictionary::Iterator Dictionary::end() const
{
    return Iterator(this, size());
}

void Dictionary::create()
{
    if (!m_valid && m_obj.is_valid()) {
        m_keys->create();
        m_values->create();
        m_valid = true;
    }
}

std::pair<Dictionary::Iterator, bool> Dictionary::insert(Mixed key, Mixed value)
{
    create();
    auto match = lower_bound_mixed(m_keys.get(), key);

    if (match.second) {
        m_values->set(match.first, value);
    }
    else {
        m_keys->insert(match.first, key);
        m_values->insert(match.first, value);
    }
    m_obj.bump_content_version();

    return {Iterator(this, match.first), !match.second};
}

auto Dictionary::operator[](Mixed key) -> MixedRef
{
    create();
    auto match = lower_bound_mixed(m_keys.get(), key);

    if (!match.second) {
        // Key not found - insert {key, {}}
        m_keys->insert(match.first, key);
        m_values->insert(match.first, {});
    }

    return {*this, match.first};
}

void Dictionary::clear()
{
    if (size() > 0) {
        m_keys->clear();
        m_values->clear();
    }
}

void Dictionary::init_from_parent() const
{
    m_valid = m_keys->init_from_parent();
    m_values->init_from_parent();
    update_content_version();
}

void Dictionary::update_child_ref(size_t ndx, ref_type new_ref)
{
    m_obj.set_dict_ref(m_col_key, ndx, new_ref);
}

ref_type Dictionary::get_child_ref(size_t ndx) const noexcept
{
    try {
        return m_obj.get_dict_ref(m_col_key, ndx);
    }
    catch (const KeyNotFound&) {
        return ref_type(0);
    }
}

std::pair<ref_type, size_t> Dictionary::get_to_dot_parent(size_t) const
{
    // TODO
    return {};
}

/************************* Dictionary::Iterator *************************/

Dictionary::Iterator::pointer Dictionary::Iterator::operator->()
{
    REALM_ASSERT(m_pos < m_keys.size());
    m_val = std::make_pair(m_keys.get(m_pos), m_values.get(m_pos));
    return &m_val;
}

} // namespace realm
