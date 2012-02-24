#ifndef __RPC_SEMILATTICE_JOIN_VCLOCK_TCC__
#define __RPC_SEMILATTICE_JOIN_VCLOCK_TCC__

#include "rpc/semilattice/joins/vclock.hpp"
#include "stl_utils.hpp"

template <class T>
vclock_t<T>::vclock_t(const stamped_value_t &_value) {
    values.insert(_value);
}

template <class T>
void vclock_t<T>::cull_old_values() {
    rassert(!values.empty(), "As a precondition, values should never be empty\n");
    typedef vclock_t<T>::value_map_t value_map_t;
    value_map_t to_delete;

    cartesian_product_iterator_t<value_map_t, value_map_t> pairs_iterator(values.begin(), values.end(), values.begin(), values.end());

    boost::optional<std::pair<typename value_map_t::iterator, typename value_map_t::iterator> > pair;
    while ((pair = *pairs_iterator)) {
        if (vclock_details::dominates(pair->first->first, pair->second->first)) {
            to_delete.insert(*pair->first);
        }
        pairs_iterator++;
    }

    for (typename value_map_t::iterator d_it =  to_delete.begin();
                                        d_it != to_delete.end();
                                        d_it++) {
        values.erase(d_it->first);
    }
    rassert(!values.empty(), "As a postcondition, values should never be empty\n");
}

template <class T>
vclock_t<T>::vclock_t() { 
    values.insert(std::make_pair(vclock_details::version_map_t(), T()));
}

template <class T>
vclock_t<T>::vclock_t(const T &_t, const boost::uuids::uuid &us) { 
    stamped_value_t tmp = std::make_pair(vclock_details::version_map_t(), _t);
    tmp.first[us] = 1;
    values.insert(tmp);
}

template <class T>
bool vclock_t<T>::in_conflict() const {
    rassert(!values.empty());
    return values.size() != 1;
}

template <class T>
void vclock_t<T>::throw_if_conflict() const {
    if (in_conflict()) {
        throw in_conflict_exc_t();
    }
}

template <class T>
vclock_t<T> vclock_t<T>::make_new_version(T t, const boost::uuids::uuid &us) {
    throw_if_conflict();
    stamped_value_t tmp = *values.begin();
    get_with_default(tmp.first, us, 0)++;
    tmp.second = t;
    return vclock_t<T>(tmp);
}

template <class T>
vclock_t<T> vclock_t<T>::make_resolving_version(T t, const boost::uuids::uuid &us) {
    vclock_details::version_map_t vmap; //construct a vmap that dominates all the others

    for (typename value_map_t::iterator it  = values.begin();
                                        it != values.end();
                                        it++) {
        vmap = vclock_details::vmap_max(vmap, it->first);
    }

    get_with_default(vmap, us, 0)++;

    return vclock_t(std::make_pair(vmap, t));
}

template <class T>
void vclock_t<T>::upgrade_version(const boost::uuids::uuid &us) {
    throw_if_conflict();

    stamped_value_t tmp = *values.begin();
    get_with_default(tmp.first, us, 0)++;
    values.clear();
    values.insert(tmp);
}

template <class T>
T vclock_t<T>::get() const {
    throw_if_conflict();
    return values.begin()->second;
}

template <class T>
T &vclock_t<T>::get_mutable() {
    throw_if_conflict();
    return values.begin()->second;
}

//semilattice concept for vclock_t
template <class T>
bool operator==(const vclock_t<T> &a, const vclock_t<T> &b) {
    return a.values == b.values;
}

template <class T>
void semilattice_join(vclock_t<T> *a, const vclock_t<T> &b) {
    a->values.insert(b.values.begin(), b.values.end());

    a->cull_old_values();
}

#endif
