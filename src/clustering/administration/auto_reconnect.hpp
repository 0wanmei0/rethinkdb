#ifndef CLUSTERING_ADMINISTRATION_AUTO_RECONNECT_HPP_
#define CLUSTERING_ADMINISTRATION_AUTO_RECONNECT_HPP_

#include <map>

#include "clustering/administration/machine_metadata.hpp"
#include "concurrency/watchable.hpp"
#include "rpc/connectivity/cluster.hpp"
#include "rpc/semilattice/view.hpp"

class auto_reconnector_t {
public:
    auto_reconnector_t(
        connectivity_cluster_t *connectivity_cluster,
        connectivity_cluster_t::run_t *connectivity_cluster_run,
        const clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > &machine_id_translation_table,
        const boost::shared_ptr<semilattice_read_view_t<machines_semilattice_metadata_t> > &machine_metadata);

private:
    void on_connect_or_disconnect();

    /* `try_reconnect()` runs in a coroutine. It automatically terminates
    itself if the peer reconnects. */
    void try_reconnect(machine_id_t machine, peer_address_t last_known_address, auto_drainer_t::lock_t drainer);

    void pulse_if_machine_declared_dead(machine_id_t machine, cond_t *c);
    void pulse_if_machine_reconnected(machine_id_t machine, cond_t *c);

    connectivity_cluster_t *connectivity_cluster;
    connectivity_cluster_t::run_t *connectivity_cluster_run;
    clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > machine_id_translation_table;
    boost::shared_ptr<semilattice_read_view_t<machines_semilattice_metadata_t> > machine_metadata;

    /* this is so that `on_disconnect()` can find the machine ID and last
    known address of a peer that just disconnected. */
    std::map<peer_id_t, std::pair<machine_id_t, peer_address_t> > connected_peers;

    auto_drainer_t drainer;

    typename watchable_t<std::map<peer_id_t, machine_id_t> >::subscription_t machine_id_translation_table_subs;
};

#endif /* CLUSTERING_ADMINISTRATION_AUTO_RECONNECT_HPP_ */
