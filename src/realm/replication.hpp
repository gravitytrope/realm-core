/*************************************************************************
 *
 * REALM CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2012] Realm Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of Realm Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to Realm Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from Realm Incorporated.
 *
 **************************************************************************/
#ifndef REALM_REPLICATION_HPP
#define REALM_REPLICATION_HPP

#include <algorithm>
#include <limits>
#include <memory>
#include <exception>
#include <string>
#include <ostream>

#include <realm/util/assert.hpp>
#include <realm/util/tuple.hpp>
#include <realm/util/safe_int_ops.hpp>
#include <realm/util/buffer.hpp>
#include <realm/util/string_buffer.hpp>
#include <realm/history.hpp>
#include <realm/impl/transact_log.hpp>

#include <iostream>

namespace realm {

// FIXME: Be careful about the possibility of one modification functions being called by another where both do transaction logging.

// FIXME: The current table/subtable selection scheme assumes that a TableRef of a subtable is not accessed after any modification of one of its ancestor tables.

// FIXME: Checking on same Table* requires that ~Table checks and nullifies on match. Another option would be to store m_selected_table as a TableRef. Yet another option would be to assign unique identifiers to each Table instance vial Allocator. Yet another option would be to explicitely invalidate subtables recursively when parent is modified.

/// Replication is enabled by passing an instance of an implementation
/// of this class to the SharedGroup constructor.
class Replication: public _impl::TransactLogConvenientEncoder, protected _impl::TransactLogStream {
public:
    // Be sure to keep this type aligned with what is actually used in
    // SharedGroup.
    using version_type = History::version_type;
    using InputStream = _impl::NoCopyInputStream;
    class TransactLogApplier;
    class Interrupted; // Exception
    class SimpleIndexTranslator;

    std::string get_database_path();

    /// Reset transaction logs. This call informs the commitlog subsystem of
    /// the initial version chosen as part of establishing a sharing scheme
    /// (also called a "session").
    /// Following a crash, the commitlog subsystem may hold multiple commitlogs
    /// for versions which are lost during the crash. When SharedGroup establishes
    /// a sharing scheme it will continue from the last version commited to
    /// the database.
    ///
    /// The call also indicates that the current thread (and current process)
    /// has exclusive access to the commitlogs, allowing them to reset
    /// synchronization variables. This can be beneficial on systems without
    /// proper support for robust mutexes.
    virtual void reset_log_management(version_type last_version);

    /// Cleanup, remove any log files
    virtual void stop_logging();

    /// The commitlog subsystem can be operated in either of two modes:
    /// server-synchronization mode and normal mode.
    /// When operating in server-synchronization mode.
    /// - the log files are persisted in a crash safe fashion
    /// - when a sharing scheme is established, the logs are assumed to exist already
    ///   (unless we are creating a new database), and an exception is thrown if they
    ///   are missing.
    /// - even after a crash which leaves the log files out of sync wrt to the database,
    ///   the log files can re-synchronized transparently
    /// When operating in normal-mode
    /// - the log files are not updated in a crash safe way
    /// - the log files are removed when the session ends
    /// - the log files are not assumed to be there when a session starts, but are
    ///   created on demand.
    virtual bool is_in_server_synchronization_mode();

    /// Called by SharedGroup during a write transaction, when readlocks are
    /// recycled, to keep the commit log management in sync with what versions
    /// can possibly be interesting in the future.
    virtual void set_last_version_seen_locally(version_type last_seen_version_number)
        REALM_NOEXCEPT;

    /// Acquire permision to start a new 'write' transaction. This
    /// function must be called by a client before it requests a
    /// 'write' transaction. This ensures that the local shared
    /// database is up-to-date. During the transaction, all
    /// modifications must be posted to this Replication instance as
    /// calls to set_value() and friends. After the completion of the
    /// transaction, the client must call either
    /// commit_write_transact() or rollback_write_transact().
    ///
    /// \throw Interrupted If this call was interrupted by an
    /// asynchronous call to interrupt().
    void begin_write_transact(SharedGroup&);

    /// Commit the accumulated transaction log. The transaction log
    /// may not be committed if any of the functions that submit data
    /// to it, have failed or been interrupted. This operation will
    /// block until the local coordinator reports that the transaction
    /// log has been dealt with in a manner that makes the transaction
    /// persistent. This operation may be interrupted by an
    /// asynchronous call to interrupt().
    ///
    /// \throw Interrupted If this call was interrupted by an
    /// asynchronous call to interrupt().
    ///
    /// FIXME: In general the transaction will be considered complete
    /// even if this operation is interrupted. Is that ok?
    version_type commit_write_transact(SharedGroup&, version_type orig_version);

    /// Called by a client to discard the accumulated transaction
    /// log. This function must be called if a write transaction was
    /// successfully initiated, but one of the functions that submit
    /// data to the transaction log has failed or has been
    /// interrupted. It must also be called after a failed or
    /// interrupted call to commit_write_transact().
    void rollback_write_transact(SharedGroup&) REALM_NOEXCEPT;

    /// Interrupt any blocking call to a function in this class. This
    /// function may be called asyncronously from any thread, but it
    /// may not be called from a system signal handler.
    ///
    /// Some of the public function members of this class may block,
    /// but only when it it is explicitely stated in the documention
    /// for those functions.
    ///
    /// FIXME: Currently we do not state blocking behaviour for all
    /// the functions that can block.
    ///
    /// After any function has returned with an interruption
    /// indication, the only functions that may safely be called are
    /// rollback_write_transact() and the destructor. If a client,
    /// after having received an interruption indication, calls
    /// rollback_write_transact() and then clear_interrupt(), it may
    /// resume normal operation through this Replication instance.
    void interrupt() REALM_NOEXCEPT;

    /// May be called by a client to reset this replication instance
    /// after an interrupted transaction. It is not an error to call
    /// this function in a situation where no interruption has
    /// occured.
    void clear_interrupt() REALM_NOEXCEPT;

    /// Called by the local coordinator to apply a transaction log
    /// received from another local coordinator.
    ///
    /// \param apply_log If specified, and the library was compiled in
    /// debug mode, then a line describing each individual operation
    /// is writted to the specified stream.
    ///
    /// \throw BadTransactLog If the transaction log could not be
    /// successfully parsed, or ended prematurely.
    static void apply_changeset(InputStream& transact_log, Group& target,
                                std::ostream* apply_log = 0);

    virtual ~Replication() REALM_NOEXCEPT {}

protected:
    Replication();

    virtual std::string do_get_database_path() = 0;

    /// As part of the initiation of a write transaction, this method
    /// is supposed to update `m_transact_log_free_begin` and
    /// `m_transact_log_free_end` such that they refer to a (possibly
    /// empty) chunk of free space.
    virtual void do_begin_write_transact(SharedGroup&) = 0;

    /// The caller guarantees that `m_transact_log_free_begin` marks
    /// the end of payload data in the transaction log.
    virtual version_type do_commit_write_transact(SharedGroup&, version_type orig_version) = 0;

    virtual void do_interrupt() REALM_NOEXCEPT = 0;

    virtual void do_clear_interrupt() REALM_NOEXCEPT = 0;

    // Part of a temporary ugly hack to avoid generating new
    // transaction logs during application of ones that have olready
    // been created elsewhere. See
    // ReplicationImpl::do_begin_write_transact() in
    // realm/replication/simplified/provider.cpp for more on this.
    static void set_replication(Group&, Replication*) REALM_NOEXCEPT;

    /// Must be called only from do_begin_write_transact(),
    /// do_commit_write_transact().
    static version_type get_current_version(SharedGroup&);

    friend class _impl::TransactReverser;
};

// re server_version: This field is written by Sync (if enabled) on commits which
// are foreign. It is carried over as part of a commit, allowing other threads involved
// with Sync to observet it. For local commits, the value of server_version is taken
// from any previous forewign commmit.


class Replication::Interrupted: public std::exception {
public:
    const char* what() const REALM_NOEXCEPT_OR_NOTHROW override
    {
        return "Interrupted";
    }
};


class TrivialReplication: public Replication {
public:
    ~TrivialReplication() REALM_NOEXCEPT {}

protected:
    typedef Replication::version_type version_type;

    TrivialReplication(const std::string& database_file);

    virtual void handle_transact_log(const char* data, std::size_t size,
                                     version_type new_version) = 0;

    static void apply_changeset(const char* data, std::size_t size, SharedGroup& target,
                                std::ostream* apply_log = 0);
    void prepare_to_write();

private:
    const std::string m_database_file;
    util::Buffer<char> m_transact_log_buffer;

    std::string do_get_database_path() override;
    void do_begin_write_transact(SharedGroup&) override;
    version_type do_commit_write_transact(SharedGroup&, version_type orig_version) override;
    void do_interrupt() REALM_NOEXCEPT override;
    void do_clear_interrupt() REALM_NOEXCEPT override;
    void transact_log_reserve(std::size_t n, char** new_begin, char** new_end) override;
    void transact_log_append(const char* data, std::size_t size, char** new_begin, char** new_end) override;
    void internal_transact_log_reserve(std::size_t, char** new_begin, char** new_end);

    std::size_t transact_log_size();
};


// Implementation:

inline Replication::Replication():
    _impl::TransactLogConvenientEncoder(static_cast<_impl::TransactLogStream&>(*this))
{
}


inline std::string Replication::get_database_path()
{
    return do_get_database_path();
}

inline void Replication::reset_log_management(version_type)
{
}

inline bool Replication::is_in_server_synchronization_mode()
{
    return false;
}

inline void Replication::stop_logging()
{
}

inline void Replication::set_last_version_seen_locally(version_type) REALM_NOEXCEPT
{
}

inline void Replication::begin_write_transact(SharedGroup& sg)
{
    do_begin_write_transact(sg);
    reset_selection_caches();
}

inline Replication::version_type
Replication::commit_write_transact(SharedGroup& sg, version_type orig_version)
{
    return do_commit_write_transact(sg, orig_version);
}

inline void Replication::rollback_write_transact(SharedGroup&) REALM_NOEXCEPT
{
}

inline void Replication::interrupt() REALM_NOEXCEPT
{
    do_interrupt();
}

inline void Replication::clear_interrupt() REALM_NOEXCEPT
{
    do_clear_interrupt();
}

inline TrivialReplication::TrivialReplication(const std::string& database_file):
    m_database_file(database_file)
{
}

inline std::size_t TrivialReplication::transact_log_size()
{
    return write_position() - m_transact_log_buffer.data();
}

inline void TrivialReplication::transact_log_reserve(std::size_t n, char** new_begin, char** new_end)
{
    internal_transact_log_reserve(n, new_begin, new_end);
}

inline void TrivialReplication::internal_transact_log_reserve(std::size_t n, char** new_begin, char** new_end)
{
    char* data = m_transact_log_buffer.data();
    std::size_t size = write_position() - data;
    m_transact_log_buffer.reserve_extra(size, n);
    data = m_transact_log_buffer.data(); // May have changed
    *new_begin = data + size;
    *new_end = data + m_transact_log_buffer.size();
}

} // namespace realm

#endif // REALM_REPLICATION_HPP
