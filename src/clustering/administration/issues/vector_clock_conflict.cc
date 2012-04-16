#include "clustering/administration/issues/vector_clock_conflict.hpp"

namespace {

template<class type_t>
void check(const std::string &object_type, const boost::uuids::uuid &object_id,
        const std::string &field, const vclock_t<type_t> &vector_clock,
        std::list<clone_ptr_t<global_issue_t> > *out) {

    if (vector_clock.in_conflict()) {
        out->push_back(clone_ptr_t<global_issue_t>(
            new vector_clock_conflict_issue_t(object_type, object_id, field)
            ));
    }
}

template<class protocol_t>
void check_namespaces_for_protocol(
        const namespaces_semilattice_metadata_t<protocol_t> &namespaces,
        std::list<clone_ptr_t<global_issue_t> > *out) {

    for (typename namespaces_semilattice_metadata_t<protocol_t>::namespace_map_t::const_iterator it =
            namespaces.namespaces.begin(); it != namespaces.namespaces.end(); it++) {
        if (!it->second.is_deleted()) {
            check("namespace", it->first, "blueprint", it->second.get().blueprint, out);
            check("namespace", it->first, "primary_datacenter", it->second.get().primary_datacenter, out);
            check("namespace", it->first, "replica_affinities", it->second.get().replica_affinities, out);
            check("namespace", it->first, "ack_expectations", it->second.get().ack_expectations, out);
            check("namespace", it->first, "shards", it->second.get().shards, out);
            check("namespace", it->first, "name", it->second.get().name, out);
        }
    }
}

}   /* anonymous namespace */

std::list<clone_ptr_t<global_issue_t> > vector_clock_conflict_issue_tracker_t::get_issues() {
    cluster_semilattice_metadata_t metadata = semilattice_view->get();

    std::list<clone_ptr_t<global_issue_t> > issues;

    check_namespaces_for_protocol(metadata.memcached_namespaces, &issues);
    check_namespaces_for_protocol(metadata.dummy_namespaces, &issues);

    for (datacenters_semilattice_metadata_t::datacenter_map_t::const_iterator it =
            metadata.datacenters.datacenters.begin(); it != metadata.datacenters.datacenters.end(); it++) {
        if (!it->second.is_deleted()) {
            check("datacenter", it->first, "name", it->second.get().name, &issues);
        }
    }

    for (machines_semilattice_metadata_t::machine_map_t::const_iterator it =
            metadata.machines.machines.begin(); it != metadata.machines.machines.end(); it++) {
        if (!it->second.is_deleted()) {
            check("machine", it->first, "datacenter", it->second.get().datacenter, &issues);
            check("machine", it->first, "name", it->second.get().name, &issues);
        }
    }

    return issues;
}
