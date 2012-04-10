#ifndef CLUSTERING_ADMINISTRATION_HTTP_JSON_ADAPTERS_HPP_
#define CLUSTERING_ADMINISTRATION_HTTP_JSON_ADAPTERS_HPP_

#include "http/json/json_adapter.hpp"
#include "protocol_api.hpp"
#include "rpc/connectivity/connectivity.hpp"
#include "rpc/semilattice/joins/deletable.hpp"
#include "rpc/semilattice/joins/vclock.hpp"

/* There are a couple of semilattice structures that we need to have json
 * adaptable for the administration server, this code shouldn't go with the
 * definition of these structures because they're deep in the rpc directory
 * which generally doesn't concern itself with how people interact with its
 * structures. */

/* The vclock_t type needs a special apapter which allows it to be resolved.
 * When the vector clock goes in to conflict all of the other requests will
 * fail. */
template <class T, class ctx_t>
class json_vclock_resolver_t : public json_adapter_if_t<ctx_t> {
private:
    vclock_t<T> *target;
private:
    typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_subfields_impl(const ctx_t &);
    cJSON *render_impl(const ctx_t &);
    void apply_impl(cJSON *change, const ctx_t &);
    void reset_impl(const ctx_t &);
    void erase_impl(const ctx_t &);
    boost::shared_ptr<subfield_change_functor_t<ctx_t> > get_change_callback();
public:
    explicit json_vclock_resolver_t(vclock_t<T> *);
};

template <class T, class ctx_t>
class json_vclock_adapter_t : public json_adapter_if_t<ctx_t> {
private:
    vclock_t<T> *target;
private:
    typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_subfields_impl(const ctx_t &);
    cJSON *render_impl(const ctx_t &);
    void apply_impl(cJSON *, const ctx_t &);
    void reset_impl(const ctx_t &);
    void erase_impl(const ctx_t &);
    boost::shared_ptr<subfield_change_functor_t<ctx_t> >  get_change_callback();
public:
    explicit json_vclock_adapter_t(vclock_t<T> *);
};

//json adapter concept for vclock_t
template <class T, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(vclock_t<T> *, const ctx_t &);

template <class T, class ctx_t>
cJSON *render_as_json(vclock_t<T> *, const ctx_t &);

//Note this is not actually part of the json_adapter concept but is a special
//purpose rendering function which is needed by the json_vclock_resolver_t
template <class T, class ctx_t>
cJSON *render_all_values(vclock_t<T> *, const ctx_t &);

template <class T, class ctx_t>
void apply_json_to(cJSON *, vclock_t<T> *, const ctx_t &);

template <class T, class ctx_t>
void on_subfield_change(vclock_t<T> *, const ctx_t &);

//json adapter concept for deletable_t
template <class T, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(deletable_t<T> *, const ctx_t &);

template <class T, class ctx_t>
cJSON *render_as_json(deletable_t<T> *, const ctx_t &);

template <class T, class ctx_t>
void apply_json_to(cJSON *, deletable_t<T> *, const ctx_t &);

template <class T, class ctx_t>
void erase_json(deletable_t<T> *, const ctx_t &);

template <class T, class ctx_t>
void on_subfield_change(deletable_t<T> *, const ctx_t &);

//json adapter concept for peer_id_t
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(peer_id_t *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(peer_id_t *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, peer_id_t *, const ctx_t &);

template <class ctx_t>
void on_subfield_change(peer_id_t *, const ctx_t &);

//json adapter concept for region map
template <class protocol_t, class value_t, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(region_map_t<protocol_t, value_t> *, const ctx_t &);

template <class protocol_t, class value_t, class ctx_t>
cJSON *render_as_json(region_map_t<protocol_t, value_t> *, const ctx_t &);

template <class protocol_t, class value_t, class ctx_t>
void apply_json_to(cJSON *, region_map_t<protocol_t, value_t> *, const ctx_t &);

template <class protocol_t, class value_t, class ctx_t>
void on_subfield_change(region_map_t<protocol_t, value_t> *, const ctx_t &);

#include "clustering/administration/http/json_adapters.tcc"

#endif
