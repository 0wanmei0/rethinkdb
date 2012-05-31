#include "utils.hpp"
#include <boost/bind.hpp>
#include <boost/program_options.hpp>

#include "arch/arch.hpp"
#include "arch/runtime/starter.hpp"
#include "arch/os_signal.hpp"
#include "clustering/administration/main/command_line.hpp"
#include "clustering/administration/main/serve.hpp"
#include "clustering/administration/cli/admin_command_parser.hpp"
#include "clustering/administration/metadata.hpp"
#include "clustering/administration/persist.hpp"
#include "mock/dummy_protocol.hpp"

namespace po = boost::program_options;

static const int default_peer_port = 20300;

class host_and_port_t {
public:
    host_and_port_t(const std::string& h, int p) : host(h), port(p) { }
    std::string host;
    int port;
};

#ifndef NDEBUG
void run_rethinkdb_create(const std::string &filepath, std::string &machine_name, int port_offset, bool *result_out) {
#else
void run_rethinkdb_create(const std::string &filepath, std::string &machine_name, bool *result_out) {
#endif

    if (metadata_persistence::check_existence(filepath)) {
        printf("ERROR: The path '%s' already exists.  Delete it and try again.\n", filepath.c_str());
        *result_out = false;
        return;
    }

    machine_id_t our_machine_id = generate_uuid();
    printf("Our machine ID: %s\n", uuid_to_str(our_machine_id).c_str());

    cluster_semilattice_metadata_t metadata;

    machine_semilattice_metadata_t machine_semilattice_metadata;
    machine_semilattice_metadata.name = machine_semilattice_metadata.name.make_new_version(machine_name, our_machine_id);
#ifndef NDEBUG
    machine_semilattice_metadata.port_offset = vclock_t<int>(port_offset, our_machine_id);
#endif


    metadata.machines.machines.insert(std::make_pair(
        our_machine_id,
        machine_semilattice_metadata
        ));

    metadata_persistence::create(filepath, our_machine_id, metadata);

    printf("Created directory '%s' and a metadata file inside it.\n", filepath.c_str());

    *result_out = true;
}

std::set<peer_address_t> look_up_peers_addresses(std::vector<host_and_port_t> names) {
    std::set<peer_address_t> peers;
    for (int i = 0; i < (int)names.size(); i++) {
        peers.insert(peer_address_t(ip_address_t(names[i].host), names[i].port));
    }
    return peers;
}

void run_rethinkdb_admin(const std::vector<host_and_port_t> &joins, int client_port, const std::vector<std::string>& command_args, bool exit_on_failure, bool *result_out) {
    os_signal_cond_t sigint_cond;
    *result_out = true;
    std::string host_port;

    if (!joins.empty())
        host_port = strprintf("%s:%d", joins[0].host.c_str(), joins[0].port);

    try {
        if (command_args.empty())
            admin_command_parser_t(host_port, look_up_peers_addresses(joins), client_port, &sigint_cond).run_console(exit_on_failure);
        else if (command_args[0] == admin_command_parser_t::complete_command)
            admin_command_parser_t(host_port, look_up_peers_addresses(joins), client_port, &sigint_cond).run_completion(command_args);
        else
            admin_command_parser_t(host_port, look_up_peers_addresses(joins), client_port, &sigint_cond).parse_and_run_command(command_args);
    } catch (admin_no_connection_exc_t& ex) {
        fprintf(stderr, "%s\n", ex.what());
        fprintf(stderr, "valid --join option required to handle command, run 'rethinkdb admin help' for more information\n");
    } catch (std::exception& ex) {
        fprintf(stderr, "%s\n", ex.what());
        *result_out = false;
    }
}

void run_rethinkdb_serve(const std::string &filepath, const std::vector<host_and_port_t> &joins, int port, int client_port, bool *result_out, std::string web_assets) {

    os_signal_cond_t sigint_cond;

    if (!metadata_persistence::check_existence(filepath)) {
        printf("ERROR: The directory '%s' does not exist.  Run 'rethinkdb create -d \"%s\"' and try again.\n", filepath.c_str(), filepath.c_str());
        *result_out = false;
        return;
    }

    machine_id_t persisted_machine_id;
    cluster_semilattice_metadata_t persisted_semilattice_metadata;

    try {
        metadata_persistence::read(filepath, &persisted_machine_id, &persisted_semilattice_metadata);
    } catch (metadata_persistence::file_exc_t e) {
        printf("ERROR: Could not read metadata file: %s\n", e.what());
        *result_out = false;
        return;
    }

    *result_out = serve(filepath, look_up_peers_addresses(joins), port, client_port, persisted_machine_id, persisted_semilattice_metadata, web_assets, &sigint_cond);
}

#ifndef NDEBUG
void run_rethinkdb_porcelain(const std::string &filepath, const std::string &machine_name, int port_offset, const std::vector<host_and_port_t> &joins, int port, int client_port, bool *result_out, std::string web_assets) {
#else
void run_rethinkdb_porcelain(const std::string &filepath, const std::string &machine_name, const std::vector<host_and_port_t> &joins, int port, int client_port, bool *result_out, std::string web_assets) {
#endif

    os_signal_cond_t sigint_cond;

    printf("Checking if directory '%s' already exists...\n", filepath.c_str());
    if (metadata_persistence::check_existence(filepath)) {
        printf("It already exists.  Loading data...\n");

        machine_id_t persisted_machine_id;
        cluster_semilattice_metadata_t persisted_semilattice_metadata;
        metadata_persistence::read(filepath, &persisted_machine_id, &persisted_semilattice_metadata);

        *result_out = serve(filepath, look_up_peers_addresses(joins), port, client_port, persisted_machine_id, persisted_semilattice_metadata, web_assets, &sigint_cond);

    } else {
        printf("It does not already exist. Creating it...\n");

        machine_id_t our_machine_id = generate_uuid();
        cluster_semilattice_metadata_t semilattice_metadata;

        if (joins.empty()) {
            printf("Creating a default namespace and default data center "
                   "for your convenience. (This is because you ran 'rethinkdb' "
                   "without 'create', 'serve', or '--join', and the directory '%s' did not already exist.)\n",
                   filepath.c_str());

            datacenter_id_t datacenter_id = generate_uuid();
            datacenter_semilattice_metadata_t datacenter_metadata;
            datacenter_metadata.name = vclock_t<std::string>("Welcome", our_machine_id);
            semilattice_metadata.datacenters.datacenters.insert(std::make_pair(
                datacenter_id,
                deletable_t<datacenter_semilattice_metadata_t>(datacenter_metadata)
                ));

            /* Add ourselves as a member of the "Welcome" datacenter. */
            machine_semilattice_metadata_t our_machine_metadata;
            our_machine_metadata.datacenter = vclock_t<datacenter_id_t>(datacenter_id, our_machine_id);
            our_machine_metadata.name = vclock_t<std::string>(machine_name, our_machine_id);
#ifndef NDEBUG
            our_machine_metadata.port_offset = vclock_t<int>(port_offset, our_machine_id);
#endif

            semilattice_metadata.machines.machines.insert(std::make_pair(
                our_machine_id,
                deletable_t<machine_semilattice_metadata_t>(our_machine_metadata)
                ));

            namespace_id_t namespace_id = generate_uuid();
            namespace_semilattice_metadata_t<memcached_protocol_t> namespace_metadata;

            namespace_metadata.name = vclock_t<std::string>("Welcome", our_machine_id);
            namespace_metadata.port = vclock_t<int>(11213, our_machine_id);

            persistable_blueprint_t<memcached_protocol_t> blueprint;
            std::map<key_range_t, blueprint_details::role_t> roles;
            roles.insert(std::make_pair(key_range_t::universe(), blueprint_details::role_primary));
            blueprint.machines_roles.insert(std::make_pair(our_machine_id, roles));
            namespace_metadata.blueprint = vclock_t<persistable_blueprint_t<memcached_protocol_t> >(blueprint, our_machine_id);

            namespace_metadata.primary_datacenter = vclock_t<datacenter_id_t>(datacenter_id, our_machine_id);

            std::map<datacenter_id_t, int> affinities;
            affinities.insert(std::make_pair(datacenter_id, 0));
            namespace_metadata.replica_affinities = vclock_t<std::map<datacenter_id_t, int> >(affinities, our_machine_id);

            std::map<datacenter_id_t, int> ack_expectations;
            ack_expectations.insert(std::make_pair(datacenter_id, 1));
            namespace_metadata.ack_expectations = vclock_t<std::map<datacenter_id_t, int> >(ack_expectations, our_machine_id);

            std::set< hash_region_t<key_range_t> > shards;
            shards.insert(hash_region_t<key_range_t>::universe());
            namespace_metadata.shards = vclock_t<std::set<hash_region_t<key_range_t> > >(shards, our_machine_id);

            semilattice_metadata.memcached_namespaces.namespaces.insert(std::make_pair(namespace_id, namespace_metadata));

        } else {

            machine_semilattice_metadata_t our_machine_metadata;
            our_machine_metadata.name = vclock_t<std::string>(machine_name, our_machine_id);
#ifndef NDEBUG
            our_machine_metadata.port_offset = vclock_t<int>(port_offset, our_machine_id);
#endif

            semilattice_metadata.machines.machines.insert(std::make_pair(
                our_machine_id,
                our_machine_metadata
                ));
        }

        metadata_persistence::create(filepath, our_machine_id, semilattice_metadata);

        *result_out = serve(filepath, look_up_peers_addresses(joins), port, client_port, our_machine_id, semilattice_metadata, web_assets, &sigint_cond);
    }
}

po::options_description get_machine_options() {
    po::options_description desc("Machine name options");
    desc.add_options()
#ifndef NDEBUG
        ("port-offset,o", po::value<int>()->default_value(0), "This machine will set up parsers for namespaces on the namespace's port + this value.")
#endif
        ("name,n", po::value<std::string>()->default_value("NN"), "The name for this machine (as will appear in the metadata.");

    return desc;
}

po::options_description get_file_option() {
    po::options_description desc("File path options");
    desc.add_options()
        ("directory,d", po::value<std::string>()->default_value("rethinkdb_cluster_data"), "specify directory to store data and metadata");
    return desc;
}

/* This allows `host_and_port_t` to be used as a command-line argument with
`boost::program_options`. */
void validate(boost::any& value_out, const std::vector<std::string>& words,
        host_and_port_t *, int)
{
    po::validators::check_first_occurrence(value_out);
    const std::string& word = po::validators::get_single_string(words);
    size_t colon_loc = word.find_first_of(':');
    if (colon_loc == std::string::npos) {
        throw po::validation_error(po::validation_error::invalid_option_value, word);
    } else {
        std::string host = word.substr(0, colon_loc);
        int port = atoi(word.substr(colon_loc + 1).c_str());
        if (host.size() == 0 || port == 0) {
            throw po::validation_error(po::validation_error::invalid_option_value, word);
        }
        value_out = host_and_port_t(host, port);
    }
}

po::options_description get_network_options() {
    po::options_description desc("Network options");
    desc.add_options()
        ("port", po::value<int>()->default_value(default_peer_port), "port for communicating with other nodes")
#ifndef NDEBUG
        ("client-port", po::value<int>()->default_value(0), "port to use when connecting to other nodes")
#endif
        ("join,j", po::value<std::vector<host_and_port_t> >()->composing(), "host:port of a node that we will connect to");
    return desc;
}

po::options_description get_rethinkdb_create_options() {
    po::options_description desc("Allowed options");
    desc.add(get_file_option());
    desc.add(get_machine_options());
    return desc;
}

po::options_description get_rethinkdb_serve_options() {
    po::options_description desc("Allowed options");
    desc.add(get_file_option());
    desc.add(get_network_options());
    return desc;
}

po::options_description get_rethinkdb_admin_options() {
    po::options_description desc("Allowed options");
    desc.add_options()
#ifndef NDEBUG
        ("client-port", po::value<int>()->default_value(0), "port to use when connecting to other nodes")
#endif
        ("join,j", po::value<std::vector<host_and_port_t> >()->composing(), "host:port of a node that we will connect to")
        ("exit-failure,x", po::value<bool>()->zero_tokens(), "exit with an error code immediately if a command fails");
    return desc;
}

po::options_description get_rethinkdb_porcelain_options() {
    po::options_description desc("Allowed options");
    desc.add(get_file_option());
    desc.add(get_machine_options());
    desc.add(get_network_options());
    return desc;
}

int main_rethinkdb_create(int argc, char *argv[]) {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, get_rethinkdb_create_options()), vm);
    po::notify(vm);

    std::string filepath = vm["directory"].as<std::string>();
    std::string machine_name = vm["name"].as<std::string>();
#ifndef NDEBUG
    int port_offset = vm["port-offset"].as<int>();
#endif

    bool result;
#ifndef NDEBUG
    run_in_thread_pool(boost::bind(&run_rethinkdb_create, filepath, machine_name, port_offset, &result));
#else
    run_in_thread_pool(boost::bind(&run_rethinkdb_create, filepath, machine_name, &result));
#endif

    return result ? 0 : 1;
}

int main_rethinkdb_serve(int argc, char *argv[]) {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, get_rethinkdb_serve_options()), vm);
    po::notify(vm);

    std::string filepath = vm["directory"].as<std::string>();
    std::vector<host_and_port_t> joins;
    if (vm.count("join") > 0) {
        joins = vm["join"].as<std::vector<host_and_port_t> >();
    }
    int port = vm["port"].as<int>();
#ifndef NDEBUG
    int client_port = vm["client-port"].as<int>();
#else
    int client_port = 0;
#endif

    path_t web_path = parse_as_path(argv[0]);
    web_path.nodes.pop_back();
    web_path.nodes.push_back("web");

    bool result;
    run_in_thread_pool(boost::bind(&run_rethinkdb_serve, filepath, joins, port, client_port, &result, render_as_path(web_path)));

    return result ? 0 : 1;
}

int main_rethinkdb_admin(int argc, char *argv[]) {
    bool result(false);
    po::variables_map vm;
    po::options_description options;
    options.add(get_rethinkdb_admin_options());

    try {
        po::command_line_parser parser(argc - 1, &argv[1]);
        parser.options(options);
        parser.allow_unregistered();
        po::parsed_options parsed = parser.run();
        po::store(parsed, vm);
        po::notify(vm);

        std::vector<host_and_port_t> joins;
        if (vm.count("join") > 0) {
            joins = vm["join"].as<std::vector<host_and_port_t> >();
        }
#ifndef NDEBUG
        int client_port = vm["client-port"].as<int>();
#else
        int client_port = 0;
#endif
        bool exit_on_failure = false;
        if (vm.count("exit-failure") > 0)
            exit_on_failure = true;

        std::vector<std::string> cmd_args = po::collect_unrecognized(parsed.options, po::include_positional); 

        // This is an ugly hack, but it seems boost will ignore an empty flag at the end, which is very useful for completions
        std::string last_arg(argv[argc - 1]);
        if (last_arg == "-" || last_arg == "--")
            cmd_args.push_back(last_arg);

        run_in_thread_pool(boost::bind(&run_rethinkdb_admin, joins, client_port, cmd_args, exit_on_failure, &result));

    } catch (std::exception& ex) {
        printf("%s\n", ex.what());
        result = false;
    }

    return result ? 0 : 1;
}

int main_rethinkdb_porcelain(int argc, char *argv[]) {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, get_rethinkdb_porcelain_options()), vm);
    po::notify(vm);

    std::string filepath = vm["directory"].as<std::string>();
    std::string machine_name = vm["name"].as<std::string>();
#ifndef NDEBUG
    int port_offset = vm["port-offset"].as<int>();
#endif
    std::vector<host_and_port_t> joins;
    if (vm.count("join") > 0) {
        joins = vm["join"].as<std::vector<host_and_port_t> >();
    }
    int port = vm["port"].as<int>();
#ifndef NDEBUG
    int client_port = vm["client-port"].as<int>();
#else
    int client_port = 0;
#endif

    path_t web_path = parse_as_path(argv[0]);
    web_path.nodes.pop_back();
    web_path.nodes.push_back("web");

    bool result;
#ifndef NDEBUG
    run_in_thread_pool(boost::bind(&run_rethinkdb_porcelain, filepath, machine_name, port_offset, joins, port, client_port, &result, render_as_path(web_path)));
#else
    run_in_thread_pool(boost::bind(&run_rethinkdb_porcelain, filepath, machine_name, joins, port, client_port, &result, render_as_path(web_path)));
#endif

    return result ? 0 : 1;
}

void help_rethinkdb_create() {
    printf("'rethinkdb create' is used to prepare a directory to act "
           "as the storage location for a RethinkDB cluster node.\n");
    std::stringstream sstream;
    sstream << get_rethinkdb_create_options();
    printf("%s\n", sstream.str().c_str());
}

void help_rethinkdb_serve() {
    printf("'rethinkdb serve' is the actual process for a RethinkDB cluster node.\n");
    std::stringstream sstream;
    sstream << get_rethinkdb_serve_options();
    printf("%s\n", sstream.str().c_str());
}

