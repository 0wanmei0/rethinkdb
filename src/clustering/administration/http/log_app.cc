#include "clustering/administration/http/log_app.hpp"

#include "arch/timing.hpp"
#include "clustering/administration/machine_id_to_peer_id.hpp"

template <class ctx_t>
cJSON *render_as_json(log_message_t *message, const ctx_t &) {
    scoped_cJSON_t json(cJSON_CreateObject());
    cJSON_AddItemToObject(json.get(), "timestamp", cJSON_CreateNumber(message->timestamp));
    cJSON_AddItemToObject(json.get(), "uptime", cJSON_CreateNumber(message->uptime.tv_sec + message->uptime.tv_nsec / 1000000000.0));
    cJSON_AddItemToObject(json.get(), "level", cJSON_CreateString(format_log_level(message->level).c_str()));
    cJSON_AddItemToObject(json.get(), "message", cJSON_CreateString(message->message.c_str()));
    return json.release();
}

log_http_app_t::log_http_app_t(
        mailbox_manager_t *mm,
        const clone_ptr_t<watchable_t<std::map<peer_id_t, log_server_business_card_t> > > &lmv,
        const clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > &mitt) :
    mailbox_manager(mm),
    log_mailbox_view(lmv),
    machine_id_translation_table(mitt)
    { }

http_res_t log_http_app_t::handle(const http_req_t &req) {
    http_req_t::resource_t::iterator it = req.resource.begin();
    if (it == req.resource.end()) {
        return http_res_t(404);
    }
    std::string machine_id_str = *it;
    it++;
    if (it != req.resource.end()) {
        return http_res_t(404);
    }

    std::vector<machine_id_t> machine_ids;
    std::map<peer_id_t, machine_id_t> all_machines = machine_id_translation_table->get();
    if (machine_id_str == "_") {
        for (std::map<peer_id_t, machine_id_t>::iterator it = all_machines.begin(); it != all_machines.end(); it++) {
            machine_ids.push_back(it->second);
        }
    } else {
        const char *p = machine_id_str.c_str(), *start = p;
        while (true) {
            while (*p && *p != '+') p++;
            try {
                machine_ids.push_back(str_to_uuid(std::string(start, p - start)));
            } catch (std::runtime_error) {
                return http_res_t(404);
            }
            if (!*p) {
                break;
            } else {
                /* Step over the `+` */
                p++;
            }
        }
    }

    std::vector<peer_id_t> peer_ids;
    for (std::vector<machine_id_t>::iterator it = machine_ids.begin(); it != machine_ids.end(); it++) {
        peer_id_t pid = machine_id_to_peer_id(*it, machine_id_translation_table->get());
        if (pid.is_nil()) {
            return http_res_t(404);
        }
        peer_ids.push_back(pid);
    }

    scoped_cJSON_t map_to_fill(cJSON_CreateObject());

    cond_t non_interruptor;
    pmap(peer_ids.size(), boost::bind(
        &log_http_app_t::fetch_logs, this, _1,
        machine_ids, peer_ids,
        100, 0, time(NULL),
        map_to_fill.get(),
        &non_interruptor));

    http_res_t res(200);
    res.set_body("application/json", cJSON_print_std_string(map_to_fill.get()));
    return res;
}

void log_http_app_t::fetch_logs(int i,
        const std::vector<machine_id_t> &machines, const std::vector<peer_id_t> &peers,
        int max_messages, time_t min_timestamp, time_t max_timestamp,
        cJSON *map_to_fill,
        signal_t *interruptor) THROWS_NOTHING {
    std::map<peer_id_t, log_server_business_card_t> bcards = log_mailbox_view->get();
    std::string key = uuid_to_str(machines[i]);
    if (bcards.count(peers[i]) == 0) {
        cJSON_AddItemToObject(map_to_fill, key.c_str(), cJSON_CreateString("lost contact with peer while fetching log"));
    }
    try {
        std::vector<log_message_t> messages = fetch_log_file(
            mailbox_manager, bcards[peers[i]],
            max_messages, min_timestamp, max_timestamp,
            interruptor);
        cJSON_AddItemToObject(map_to_fill, key.c_str(), render_as_json(&messages, 0));
    } catch (interrupted_exc_t) {
        /* ignore */
    } catch (std::runtime_error e) {
        cJSON_AddItemToObject(map_to_fill, key.c_str(), cJSON_CreateString(e.what()));
    } catch (resource_lost_exc_t) {
        cJSON_AddItemToObject(map_to_fill, key.c_str(), cJSON_CreateString("lost contact with peer while fetching log"));
    }
}
