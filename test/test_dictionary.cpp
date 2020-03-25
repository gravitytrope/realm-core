/*************************************************************************
 *
 * Copyright 2018 Realm Inc.
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

#include "testsettings.hpp"

#include <realm.hpp>
#include <realm/dictionary.hpp>
#include <realm/group.hpp>
#include <realm/history.hpp>

#include "test.hpp"

using namespace realm;
using namespace realm::test_util;

// Test independence and thread-safety
// -----------------------------------
//
// All tests must be thread safe and independent of each other. This
// is required because it allows for both shuffling of the execution
// order and for parallelized testing.
//
// In particular, avoid using std::rand() since it is not guaranteed
// to be thread safe. Instead use the API offered in
// `test/util/random.hpp`.
//
// All files created in tests must use the TEST_PATH macro (or one of
// its friends) to obtain a suitable file system path. See
// `test/util/test_path.hpp`.
//
//
// Debugging and the ONLY() macro
// ------------------------------
//
// A simple way of disabling all tests except one called `Foo`, is to
// replace TEST(Foo) with ONLY(Foo) and then recompile and rerun the
// test suite. Note that you can also use filtering by setting the
// environment varible `UNITTEST_FILTER`. See `README.md` for more on
// this.
//
// Another way to debug a particular test, is to copy that test into
// `experiments/testcase.cpp` and then run `sh build.sh
// check-testcase` (or one of its friends) from the command line.

TEST(Dictionary_Basics)
{
    Group g;
    auto cmp = [this](Mixed x, Mixed y) { CHECK_EQUAL(x, y); };

    auto foo = g.add_table("foo");
    auto col_dict = foo->add_column_dictionary("dictionaries");

    Obj obj1 = foo->create_object();
    Obj obj2 = foo->create_object();

    {
        Dictionary dict = obj1.get_dictionary(col_dict);
        CHECK(dict.insert("Hello", 9).second);
        CHECK_EQUAL(dict.get("Hello").get_int(), 9);
        CHECK_NOT(dict.insert("Hello", 10).second);
        CHECK_EQUAL(dict.get("Hello").get_int(), 10);
        dict["Goodbye"] = "cruel world";
        CHECK_EQUAL(dict.get("Goodbye").get_string(), "cruel world");
        CHECK_THROW_ANY(dict.get("Baa").get_string()); // Within range
        CHECK_THROW_ANY(dict.get("Foo").get_string()); // Outside range
    }
    {
        Dictionary dict = obj1.get_dictionary(col_dict);
        cmp(dict.get("Hello"), 10);
        cmp(dict["Goodbye"], "cruel world");
    }
    {
        Dictionary dict = obj2.get_dictionary(col_dict);
        CHECK_EQUAL(dict.size(), 0);
        CHECK_THROW_ANY(dict.get("Baa").get_string());
    }
}

TEST(Dictionary_Transaction)
{
    SHARED_GROUP_TEST_PATH(path);
    auto hist = make_in_realm_history(path);
    DBRef db = DB::create(*hist);
    ObjKey k0;
    ColKey col_dict;
    auto cmp = [this](Mixed x, Mixed y) { CHECK_EQUAL(x, y); };

    auto rt = db->start_read();
    {
        WriteTransaction wt(db);
        auto foo = wt.add_table("foo");
        col_dict = foo->add_column_dictionary("dictionaries");

        Obj obj1 = foo->create_object();
        Obj obj2 = foo->create_object();
        Dictionary dict = obj1.get_dictionary(col_dict);
        k0 = obj1.get_key();
        dict.insert("Hello", 9);
        dict["Goodbye"] = "cruel world";

        wt.commit();
    }
    rt->advance_read();
    ConstTableRef table = rt->get_table("foo");
    Dictionary dict;
    dict = table->get_object(k0).get_dictionary(col_dict);
    cmp(dict.get("Hello"), 9);
    cmp(dict.get("Goodbye"), "cruel world");
    {
        WriteTransaction wt(db);
        auto foo = wt.get_table("foo");
        Dictionary d = foo->get_object(k0).get_dictionary(col_dict);
        d["Good morning"] = "sunshine";

        wt.commit();
    }
    rt->advance_read();
    // rt->to_json(std::cout);
    cmp(dict.get("Good morning"), "sunshine");
}
