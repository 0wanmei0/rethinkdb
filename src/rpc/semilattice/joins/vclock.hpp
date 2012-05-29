#ifndef RPC_SEMILATTICE_JOINS_VCLOCK_HPP_
#define RPC_SEMILATTICE_JOINS_VCLOCK_HPP_

#include "errors.hpp"
#include <boost/uuid/uuid.hpp>

#include "containers/map_sentries.hpp"
#include "http/json.hpp"
#include "rpc/serialize_macros.hpp"

namespace vclock_details {
typedef std::map<boost::uuids::uuid, int> version_map_t;

bool dominates(const version_map_t &, const version_map_t &);

version_map_t vmap_max(const version_map_t &, const version_map_t &);

void print_version_map(const version_map_t &);
} //namespace vclock_details

class in_conflict_exc_t : public std::exception {
public:
    const char *what() const throw () {
        return "Tried to access a vector clock protected value that was in conflict.";
    }

    virtual ~in_conflict_exc_t() throw () { }
};


template <class T>
class vclock_t {
private:
    template <class TT, class ctx_t>
    friend cJSON *render_all_values(vclock_t<TT> *, const ctx_t &);

    template <class TT>
    friend bool operator==(const vclock_t<TT> &, const vclock_t<TT> &);

    template <class TT>
    friend void semilattice_join(vclock_t<TT> *, const vclock_t<TT> &);

    typedef std::pair<vclock_details::version_map_t, T> stamped_value_t;

    typedef std::map<vclock_details::version_map_t, T> value_map_t;
    value_map_t values;

    RDB_MAKE_ME_SERIALIZABLE_1(values);

    explicit vclock_t(const stamped_value_t &_value);

    //if there exist 2 values a,b in values s.t. a.first < b.first remove a
    void cull_old_values();

public:
    vclock_t();

    vclock_t(const T &_t, const boost::uuids::uuid &us);

    bool in_conflict() const;

    void throw_if_conflict() const;

    vclock_t<T> make_new_version(const T& t, const boost::uuids::uuid &us);

    vclock_t<T> make_resolving_version(const T& t, const boost::uuids::uuid &us);

    void upgrade_version(const boost::uuids::uuid &us);

    T get() const;

    T &get_mutable();

    std::vector<T> get_all_values() const;
};

//semilattice concept for vclock_t
template <class T>
bool operator==(const vclock_t<T> &, const vclock_t<T> &);

template <class T>
void semilattice_join(vclock_t<T> *, const vclock_t<T> &);

#include "rpc/semilattice/joins/vclock.tcc"

#endif
