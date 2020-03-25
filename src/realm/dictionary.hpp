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

#ifndef REALM_DICTIONARY_HPP
#define REALM_DICTIONARY_HPP

#include <realm/array.hpp>
#include <realm/array_mixed.hpp>
#include <realm/bplustree.hpp>

namespace realm {

class Dictionary : public ArrayParent {
public:
    class Iterator;
    class MixedRef;

    Dictionary() {}

    Dictionary(const ConstObj& obj, ColKey col_key)
        : m_obj(obj)
        , m_col_key(col_key)
        , m_keys(new BPlusTree<Mixed>(obj.get_alloc()))
        , m_values(new BPlusTree<Mixed>(obj.get_alloc()))
    {
        size_t ndx = m_obj.get_row_ndx();
        m_keys->set_parent(this, ndx * 2);
        m_values->set_parent(this, ndx * 2 + 1);
        init_from_parent();
    }
    Dictionary(const Dictionary& other)
        : Dictionary(other.m_obj, other.m_col_key)
    {
    }
    Dictionary& operator=(const Dictionary& other);

    bool is_attached() const
    {
        return m_obj.is_valid();
    }

    size_t size() const
    {
        if (!is_attached())
            return 0;
        update_if_needed();
        if (!m_valid)
            return 0;

        return m_keys->size();
    }

    void create();

    // throws std::out_of_range if key is not found
    Mixed get(Mixed key) const;
    // first points to inserted/updated element.
    // second is true if the element was inserted
    std::pair<Dictionary::Iterator, bool> insert(Mixed key, Mixed value);
    // adds entry if key is not found
    MixedRef operator[](Mixed key);

    void clear();

    Iterator begin() const;
    Iterator end() const;

private:
    friend class MixedRef;
    Obj m_obj;
    ColKey m_col_key;
    mutable bool m_valid = false;
    mutable uint_fast64_t m_content_version = 0;
    std::unique_ptr<BPlusTree<Mixed>> m_keys;
    std::unique_ptr<BPlusTree<Mixed>> m_values;

    void update_content_version() const
    {
        m_content_version = m_obj.get_alloc().get_content_version();
    }

    void update_if_needed() const
    {
        auto content_version = m_obj.get_alloc().get_content_version();
        if (m_obj.update_if_needed() || content_version != m_content_version) {
            init_from_parent();
        }
    }
    void init_from_parent() const;

    void update_child_ref(size_t ndx, ref_type new_ref) override;
    ref_type get_child_ref(size_t ndx) const noexcept override;
    std::pair<ref_type, size_t> get_to_dot_parent(size_t) const override;
};

class Dictionary::Iterator {
public:
    typedef std::forward_iterator_tag iterator_category;
    typedef const std::pair<Mixed, Mixed> value_type;
    typedef ptrdiff_t difference_type;
    typedef const value_type* pointer;
    typedef const value_type& reference;

    pointer operator->();

    reference operator*()
    {
        return *operator->();
    }

    Iterator& operator++()
    {
        m_pos++;
        return *this;
    }

    Iterator operator++(int)
    {
        Iterator tmp(*this);
        operator++();
        return tmp;
    }

    bool operator!=(const Iterator& rhs)
    {
        return m_pos != rhs.m_pos;
    }

    bool operator==(const Iterator& rhs)
    {
        return m_pos == rhs.m_pos;
    }

private:
    friend class Dictionary;

    const BPlusTree<Mixed>& m_keys;
    const BPlusTree<Mixed>& m_values;
    size_t m_pos;
    std::pair<Mixed, Mixed> m_val;

    Iterator(const Dictionary* dict, size_t pos)
        : m_keys(*dict->m_keys)
        , m_values(*dict->m_values)
        , m_pos(pos)
    {
    }
};

class Dictionary::MixedRef {
public:
    operator Mixed()
    {
        return m_dict.m_values->get(m_ndx);
    }
    MixedRef& operator=(Mixed val)
    {
        m_dict.m_values->set(m_ndx, val);
        return *this;
    }

private:
    friend class Dictionary;
    MixedRef(Dictionary& dict, size_t ndx)
        : m_dict(dict)
        , m_ndx(ndx)
    {
    }
    Dictionary& m_dict;
    size_t m_ndx;
};
} // namespace realm

#endif /* SRC_REALM_DICTIONARY_HPP_ */
