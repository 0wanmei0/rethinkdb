#ifndef __CLUSTERING_ADMINISTRATION_PERSISTABLE_BLUEPRINT_HPP__
#define __CLUSTERING_ADMINISTRATION_PERSISTABLE_BLUEPRINT_HPP__

#include "clustering/reactor/blueprint.hpp"
#include "clustering/administration/machine_metadata.hpp"

/* This is like `blueprint_t`, except that it is indexed by `machine_id_t`
instead of `peer_id_t`. This is important because peer IDs chan change when a
node restarts, but machine IDs do not. So data structures that contain peer IDs,
such as `blueprint_t`, should not be persisted. */

namespace blueprint_details {

template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(role_t *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(role_t *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, role_t *, const ctx_t &);

template <class ctx_t>
void on_subfield_change(role_t *, const ctx_t &);

} //namespace blueprint_details

template<class protocol_t>
class persistable_blueprint_t {
public:
    //TODO if we swap the region_t and peer_id_t's positions in these maps we
    //can get better data structure integrity. It might get a bit tricky
    //though.

    typedef std::map<typename protocol_t::region_t, blueprint_details::role_t> region_to_role_map_t;
    typedef std::map<machine_id_t, region_to_role_map_t> role_map_t;

    role_map_t machines_roles;

    bool operator==(const persistable_blueprint_t &other) const {
        return machines_roles == other.machines_roles;
    }

    RDB_MAKE_ME_SERIALIZABLE_1(machines_roles);
};

template <class protocol_t, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(blueprint_t<protocol_t> *, const ctx_t &);

template <class protocol_t, class ctx_t>
cJSON *render_as_json(persistable_blueprint_t<protocol_t> *, const ctx_t &);

template <class protocol_t, class ctx_t>
void apply_json_to(cJSON *, persistable_blueprint_t<protocol_t> *, const ctx_t &);

template <class protocol_t, class ctx_t>
void on_subfield_change(persistable_blueprint_t<protocol_t> *, const ctx_t &);

#include "clustering/administration/persistable_blueprint.tcc"

#endif /* __CLUSTERING_ADMINISTRATION_PERSISTABLE_BLUEPRINT_HPP__ */
