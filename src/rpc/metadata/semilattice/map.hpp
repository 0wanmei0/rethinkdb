#ifndef __RPC_METADATA_SEMILATTICE_MAP_HPP__
#define __RPC_METADATA_SEMILATTICE_MAP_HPP__

#include <map>

/* We join `std::map`s by taking their union and resolving conflicts by doing a
semilattice join on the values. */

template<class key_t, class value_t>
void semilattice_join(std::map<key_t, value_t> *a, const std::map<key_t, value_t> &b) {
    for (typename std::map<key_t, value_t>::const_iterator it = b.begin(); it != b.end(); it++) {
        typename std::map<key_t, value_t>::iterator it2 = a->find((*it).first);
        if (it2 == a->end()) {
            (*a)[(*it).first] = (*it).second;
        } else {
            semilattice_join(&(*it2).second, (*it).second);
        }
    }
}

#endif /* __RPC_METADATA_SEMILATTICE_MAP_HPP__ */
