//
//  c4Impl.hh
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Impl_h
#define c4Impl_h

#include "slice.hh"
#include "Database.hh"
#include "Collatable.hh"
#include "VersionedDocument.hh"
#include "Error.hh"
#include "LogInternal.hh"
#include <functional>

// Defining C4DB_THREADSAFE as 1 will make C4Database thread-safe: the same handle can be called
// simultaneously from multiple threads. Transactions will be single-threaded: once a thread has
// called c4db_beginTransaction, other threads making that call will block until the transaction
// ends.
#if C4DB_THREADSAFE
#include <mutex>
#endif

using namespace cbforest;

namespace cbforest {
    class VersionedDocument;
}


// Predefine C4Slice as a typedef of slice so we can use the richer slice API:

typedef slice C4Slice;

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define C4_IMPL // This tells c4.h to skip its declaration of C4Slice

#define kC4SliceNull slice::null


// INTERNAL APIs:


#include "c4Database.h"

struct C4Document;
struct C4DocEnumerator;

namespace c4Internal {

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) noexcept;
    void recordHTTPError(int httpStatus, C4Error* outError) noexcept;
    void recordError(const error &e, C4Error* outError) noexcept;
    void recordException(const std::exception &e, C4Error* outError) noexcept;
    void recordUnknownException(C4Error* outError) noexcept;
    static inline void clearError(C4Error* outError) {if (outError) outError->code = 0;}

    #define catchError(OUTERR) \
        catch (const error &err) { \
            recordError(err, OUTERR); \
        } catch (const std::exception &x) { \
            recordException(x, OUTERR); \
        } catch (...) { \
            recordUnknownException(OUTERR); \
        }

    Database::config c4DbConfig(C4DatabaseFlags flags, const C4EncryptionKey *key);

    bool rekey(Database* database, const C4EncryptionKey *newKey, C4Error *outError);

    C4Document* newC4Document(C4Database*, Document&&);

    const VersionedDocument& versionedDocument(C4Document*);

    typedef std::function<bool(const Document&,
                               uint32_t documentFlags,  // C4DocumentFlags
                               slice docType)> EnumFilter;

    void setEnumFilter(C4DocEnumerator*, EnumFilter);


    /** Base class that keeps track of the total instance count of all subclasses,
        which is returned by c4_getObjectCount(). */
    class InstanceCounted {
    public:
        static std::atomic_int gObjectCount;
        InstanceCounted()   {++gObjectCount;}
        ~InstanceCounted()  {--gObjectCount;}
    };


    /** Simple thread-safe ref-counting implementation.
        Note: The ref-count starts at 0, so you must call retain() on an instance right after
        constructing it. */
    template <typename SELF>
    struct RefCounted : InstanceCounted {

        int refCount() const { return _refCount; }

        SELF* retain() noexcept {
            ++_refCount;
            return (SELF*)this;
        }

        void release() noexcept {
            int newref = --_refCount;
            if (newref == 0)
                delete this;
            else if (newref < 0)
                Warn("RefCounted object at %p released too many times; refcount now %d",
                     this, (int)_refCount);
        }

    protected:
        virtual ~RefCounted() {
            if (_refCount > 0) {
                Warn("FATAL: RefCounted object at %p destructed while it still has a refCount of %d",
                     this, (int)_refCount);
                abort();
            }
        }
    private:
        std::atomic_int _refCount {0};
    };


    /** Simple smart pointer that retains the RefCounted instance it holds. */
    template <typename REFCOUNTED>
    class Retained {
    public:
        Retained(REFCOUNTED *t)          :_ref(t->retain()) { }
        ~Retained()                      {_ref->release();}
        operator REFCOUNTED* () const    {return _ref;}
        REFCOUNTED* operator-> () const  {return _ref;}
    private:
        REFCOUNTED *_ref;

        Retained(const Retained&) =delete;
        Retained& operator=(const Retained&) =delete;
    };


    // Internal C4EnumeratorFlags value. Includes purged docs (what ForestDB calls 'deleted').
    // Should only need to be used for the view indexer's enumerator.
    static const uint16_t kC4IncludePurged = 0x8000;
}

using namespace c4Internal;


// Structs below must be in the global namespace because they are forward-declared in the C API.

struct c4Database : public Database, RefCounted<c4Database> {
    c4Database(std::string path, const config& cfg);
    Transaction* transaction() {
        CBFAssert(_transaction);
        return _transaction;
    }
    // Transaction methods below acquire _transactionMutex. Do not call them if
    // _mutex is already locked, or deadlock may occur!
    void beginTransaction();
    bool inTransaction();
    bool mustBeInTransaction(C4Error *outError);
    bool mustNotBeInTransaction(C4Error *outError);
    bool endTransaction(bool commit);

#if C4DB_THREADSAFE
    // Mutex for synchronizing Database calls. Non-recursive!
    std::mutex _mutex;
#endif

private:
    virtual ~c4Database() { CBFAssert(_transactionLevel == 0); }
#if C4DB_THREADSAFE
    // Recursive mutex for accessing _transaction and _transactionLevel.
    // Must be acquired BEFORE _mutex, or deadlock may occur!
    std::recursive_mutex _transactionMutex;
#endif
    Transaction* _transaction {NULL};
    int _transactionLevel {0};
};


#if C4DB_THREADSAFE
#define WITH_LOCK(db) std::lock_guard<std::mutex> _lock((db)->_mutex)
#else
#define WITH_LOCK(db) do { } while (0)  // no-op
#endif


struct c4Key : public CollatableBuilder, c4Internal::InstanceCounted {
    c4Key()                 :CollatableBuilder() { }
    c4Key(C4Slice bytes)    :CollatableBuilder(bytes, true) { }
};


struct c4KeyValueList {
    std::vector<Collatable> keys;
    std::vector<alloc_slice> values;
};


#endif /* c4Impl_h */
