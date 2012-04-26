#ifndef CLUSTERING_ADMINISTRATION_ISSUES_PINNINGS_SHARDS_MISMTACH_TCC_
#define CLUSTERING_ADMINISTRATION_ISSUES_PINNINGS_SHARDS_MISMTACH_TCC_

#include "clustering/administration/http/json_adapters.hpp"
#include "http/json/json_adapter.hpp"
#include "utils.hpp"

template <class protocol_t>
pinnings_shards_mismatch_issue_t<protocol_t>::pinnings_shards_mismatch_issue_t(
        const namespace_id_t &_offending_namespace,
        const std::set<typename protocol_t::region_t> &_shards,
        const region_map_t<protocol_t, boost::uuids::uuid> &_primary_pinnings,
        const region_map_t<protocol_t, std::set<boost::uuids::uuid> > &_secondary_pinnings)
    : offending_namespace(_offending_namespace), shards(_shards),
      primary_pinnings(_primary_pinnings), secondary_pinnings(_secondary_pinnings)
{ }

template <class protocol_t>
std::string pinnings_shards_mismatch_issue_t<protocol_t>::get_description() const {
    //XXX XXX god fuck this. We have to make copies because we don't have
    //constness worked out in json_adapters and just fuck everything.
    std::set<typename protocol_t::region_t> _shards = shards;
    region_map_t<protocol_t, machine_id_t> _primary_pinnings = primary_pinnings;
    region_map_t<protocol_t, std::set<machine_id_t> > _secondary_pinnings = secondary_pinnings;
    return strprintf("The namespace: %s has a pinning map which is segmented differently than its sharding scheme.\n"
                      "Sharding scheme:\n %s\n"
                      "Primary pinnings:\n %s\n"
                      "Secondary pinnings:\n %s\n",
                      uuid_to_str(offending_namespace).c_str(),
                      cJSON_print_std_string(scoped_cJSON_t(render_as_json(&_shards, 0)).get()).c_str(),
                      cJSON_print_std_string(scoped_cJSON_t(render_as_json(&_primary_pinnings, 0)).get()).c_str(),
                      cJSON_print_std_string(scoped_cJSON_t(render_as_json(&_secondary_pinnings, 0)).get()).c_str());
}

template <class protocol_t>
cJSON *pinnings_shards_mismatch_issue_t<protocol_t>::get_json_description() {
    issue_json_t json;
    json.critical = false;
    json.description = get_description();
    json.type.issue_type = PINNINGS_SHARDS_MISMATCH;
    json.time = get_secs();
    cJSON *res = render_as_json(&json, 0);

    cJSON_AddItemToObject(res, "offending_namespace", render_as_json(&offending_namespace, 0));
    cJSON_AddItemToObject(res, "shards", render_as_json(&shards, 0));
    cJSON_AddItemToObject(res, "primary_pinnings", render_as_json(&primary_pinnings, 0));
    cJSON_AddItemToObject(res, "secondary_pinnings", render_as_json(&secondary_pinnings, 0));

    return res;
}

template <class protocol_t>
pinnings_shards_mismatch_issue_t<protocol_t> *pinnings_shards_mismatch_issue_t<protocol_t>::clone() const {
    return new pinnings_shards_mismatch_issue_t<protocol_t>(offending_namespace, shards, primary_pinnings, secondary_pinnings);
}

template <class protocol_t>
std::list<clone_ptr_t<global_issue_t> > pinnings_shards_mismatch_issue_tracker_t<protocol_t>::get_issues() {
    std::list<clone_ptr_t<global_issue_t> > res;

    typedef std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t<protocol_t> > > namespace_map_t;

    namespace_map_t namespaces = semilattice_view->get().namespaces;

    for (typename namespace_map_t::iterator it  = namespaces.begin();
                                            it != namespaces.end();
                                            ++it) {
        if (it->second.is_deleted()) {
            continue;
        }
        std::set<typename protocol_t::region_t> shards = it->second.get().shards.get();
        region_map_t<protocol_t, machine_id_t> primary_pinnings = it->second.get().primary_pinnings.get();
        region_map_t<protocol_t, std::set<machine_id_t> > secondary_pinnings = it->second.get().secondary_pinnings.get();
        for (typename std::set<typename protocol_t::region_t>::iterator shit  = shards.begin();
                                                                        shit != shards.end();
                                                                        ++shit) {
            /* Check primary pinnings for problem. */
            region_map_t<protocol_t, machine_id_t> primary_masked_pinnings = primary_pinnings.mask(*shit);

            machine_id_t primary_expected_val = primary_masked_pinnings.begin()->second;
            for (typename region_map_t<protocol_t, machine_id_t>::iterator pit  = primary_masked_pinnings.begin();
                                                                           pit != primary_masked_pinnings.end();
                                                                           ++pit) {
                if (pit->second != primary_expected_val) {
                    res.push_back(clone_ptr_t<global_issue_t>(new pinnings_shards_mismatch_issue_t<protocol_t>(it->first, shards, primary_pinnings, secondary_pinnings)));
                    goto NAMESPACE_HAS_ISSUE;
                }
            }

            /* Check secondary pinnings for problem. */
            region_map_t<protocol_t, std::set<machine_id_t> > secondary_masked_pinnings = secondary_pinnings.mask(*shit);

            std::set<machine_id_t> secondary_expected_val = secondary_masked_pinnings.begin()->second;
            for (typename region_map_t<protocol_t, std::set<machine_id_t> >::iterator pit  = secondary_masked_pinnings.begin();
                                                                                      pit != secondary_masked_pinnings.end();
                                                                                      ++pit) {
                if (pit->second!= secondary_expected_val) {
                    res.push_back(clone_ptr_t<global_issue_t>(new pinnings_shards_mismatch_issue_t<protocol_t>(it->first, shards, primary_pinnings, secondary_pinnings)));
                    goto NAMESPACE_HAS_ISSUE;
                }
            }
        }
    NAMESPACE_HAS_ISSUE:
        (void)0;
        // do nothing, continue around loop.
    }

    return res;
}

#endif  // CLUSTERING_ADMINISTRATION_ISSUES_PINNINGS_SHARDS_MISMTACH_TCC_
