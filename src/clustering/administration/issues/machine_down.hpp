#ifndef CLUSTERING_ADMINISTRATION_ISSUES_MACHINE_DOWN_HPP_
#define CLUSTERING_ADMINISTRATION_ISSUES_MACHINE_DOWN_HPP_

#include "clustering/administration/issues/json.hpp"
#include "clustering/administration/issues/global.hpp"
#include "clustering/administration/metadata.hpp"
#include "rpc/semilattice/view.hpp"

class machine_down_issue_t : public global_issue_t {
public:
    explicit machine_down_issue_t(const machine_id_t &mid) : machine_id(mid) { }

    std::string get_description() const {
        return "Machine " + uuid_to_str(machine_id) + " is inaccessible.";
    }

    cJSON *get_json_description() {
        issue_json_t json;
        json.critical = true;
        json.description = "Machine " + uuid_to_str(machine_id) + " is inaccessible.";
        json.type.issue_type = MACHINE_DOWN;
        json.time = get_secs();

        cJSON *res = render_as_json(&json, 0);
        cJSON_AddItemToObject(res, "victim", render_as_json(&machine_id, 0));

        return res;
    }

    machine_down_issue_t *clone() const {
        return new machine_down_issue_t(machine_id);
    }

    machine_id_t machine_id;

private:
    DISABLE_COPYING(machine_down_issue_t);
};

class machine_down_issue_tracker_t : public global_issue_tracker_t {
public:
    machine_down_issue_tracker_t(
            boost::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t> > semilattice_view,
            const clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > &machine_id_translation_table);

    std::list<clone_ptr_t<global_issue_t> > get_issues();

private:
    boost::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t> > semilattice_view;
    clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > machine_id_translation_table;
};

#endif /* CLUSTERING_ADMINISTRATION_ISSUES_MACHINE_DOWN_HPP_ */
