#ifndef HTTP_JSON_JSON_ADAPTER_TCC_
#define HTTP_JSON_JSON_ADAPTER_TCC_

#include "containers/uuid.hpp"
#include "utils.hpp"
#include "logger.hpp"
#include "http/json.hpp"

//implementation for subfield_functor_t
template<class T, class ctx_t>
standard_subfield_change_functor_t<T, ctx_t>::standard_subfield_change_functor_t(T *_target)
    : target(_target)
{ }

template<class T, class ctx_t>
void standard_subfield_change_functor_t<T, ctx_t>::on_change(const ctx_t &ctx) {
    on_subfield_change(target, ctx);
}

//implementation for json_adapter_if_t
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_if_t<ctx_t>::get_subfields(const ctx_t &ctx) {
    json_adapter_map_t res = get_subfields_impl(ctx);
    for (typename json_adapter_map_t::iterator it  = res.begin();
                                               it != res.end();
                                               it++) {

        it->second->superfields.insert(it->second->superfields.end(), 
                                      superfields.begin(), 
                                      superfields.end());

        it->second->superfields.push_back(get_change_callback());
    }
    return res;
}

template <class ctx_t>
cJSON *json_adapter_if_t<ctx_t>::render(const ctx_t &ctx) {
    return render_impl(ctx);
}

template <class ctx_t>
void json_adapter_if_t<ctx_t>::apply(cJSON *change, const ctx_t &ctx) {
    try {
        apply_impl(change, ctx);
    } catch (std::runtime_error e) {
        std::string s = cJSON_print_std_string(change);
        throw schema_mismatch_exc_t(strprintf("Failed to apply change: %s\n", s.c_str()));
    }
    boost::shared_ptr<subfield_change_functor_t<ctx_t> > change_callback = get_change_callback();
    if (change_callback) {
        get_change_callback()->on_change(ctx);
    }

    for (typename std::vector<boost::shared_ptr<subfield_change_functor_t<ctx_t> > >::iterator it  = superfields.begin();
                                                                                             it != superfields.end();
                                                                                             it++) {
        if (*it) {
            (*it)->on_change(ctx);
        }
    }
}

template <class ctx_t>
void json_adapter_if_t<ctx_t>::erase(const ctx_t &ctx) {
    erase_impl(ctx);

    boost::shared_ptr<subfield_change_functor_t<ctx_t> > change_callback = get_change_callback();
    if (change_callback) {
        get_change_callback()->on_change(ctx);
    }

    for (typename std::vector<boost::shared_ptr<subfield_change_functor_t<ctx_t> > >::iterator it  = superfields.begin();
                                                                                             it != superfields.end();
                                                                                             it++) {
        if (*it) {
            (*it)->on_change(ctx);
        }
    }
}

//implementation for json_adapter_t
template <class T, class ctx_t>
json_adapter_t<T, ctx_t>::json_adapter_t(T *_target)
    : target(_target)
{ }

template <class T, class ctx_t>
cJSON *json_adapter_t<T, ctx_t>::render_impl(const ctx_t &ctx) {
    return render_as_json(target, ctx);
}

template <class T, class ctx_t>
void json_adapter_t<T, ctx_t>::apply_impl(cJSON *change, const ctx_t &ctx) {
    apply_json_to(change, target, ctx);
}

template <class T, class ctx_t>
void json_adapter_t<T, ctx_t>::erase_impl(const ctx_t &ctx) {
    erase(target, ctx);
}

template <class T, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_t<T, ctx_t>::get_subfields_impl(const ctx_t &ctx) {
    return get_json_subfields(target, ctx);
}

template <class T, class ctx_t>
boost::shared_ptr<subfield_change_functor_t<ctx_t> > json_adapter_t<T, ctx_t>::get_change_callback() {
    return boost::shared_ptr<subfield_change_functor_t<ctx_t> >(new standard_subfield_change_functor_t<T, ctx_t>(target));
}

//implementation for json_read_only_adapter_t
template <class T, class ctx_t>
json_read_only_adapter_t<T, ctx_t>::json_read_only_adapter_t(T *t)
    : json_adapter_t<T, ctx_t>(t)
{ }

template <class T, class ctx_t>
void json_read_only_adapter_t<T, ctx_t>::apply_impl(cJSON *, const ctx_t &) {
    throw permission_denied_exc_t("Trying to write to a readonly value\n");
}

template <class T, class ctx_t>
void json_read_only_adapter_t<T, ctx_t>::erase_impl(const ctx_t &) {
    throw permission_denied_exc_t("Trying to erase a readonly value\n");
}


//implementation for json_temporary_adapter_t
template <class T, class ctx_t>
json_temporary_adapter_t<T, ctx_t>::json_temporary_adapter_t(const T &_t)
    : json_read_only_adapter_t<T, ctx_t>(&t), t(_t)
{ }

//implementation for map_inserter_t
template <class container_t, class ctx_t>
json_map_inserter_t<container_t, ctx_t>::json_map_inserter_t(container_t *_target, gen_function_t _generator, value_t _initial_value) 
    : target(_target), generator(_generator), initial_value(_initial_value)
{ }

template <class container_t, class ctx_t>
cJSON *json_map_inserter_t<container_t, ctx_t>::render_impl(const ctx_t &ctx) {
    /* This is perhaps a bit confusing, json_map_inserters will generally
     * render as nothing (an empty object) unless they have been used to insert
     * something, this is kind of a weird thing so that when someone does a
     * post to this value they get a view of the things we inserted. */
    cJSON *res = cJSON_CreateObject();
    for (typename keys_set_t::iterator it =  added_keys.begin();
                                       it != added_keys.end();
                                       it++) {
        scoped_cJSON_t key(render_as_json(&*it, ctx));
        cJSON_AddItemToObject(res, get_string(key.get()).c_str(), render_as_json(&(target->find(*it)->second), ctx));
    }
    return res;
}

template <class container_t, class ctx_t>
void json_map_inserter_t<container_t, ctx_t>::apply_impl(cJSON *change, const ctx_t &ctx) {
    typename container_t::key_type key = generator();
    added_keys.insert(key);

    typename container_t::mapped_type val = initial_value;
    apply_json_to(change, &val, ctx);

    target->insert(typename container_t::value_type(key, val));
}

template <class container_t, class ctx_t>
void json_map_inserter_t<container_t, ctx_t>::erase_impl(const ctx_t &) {
    throw permission_denied_exc_t("Trying to erase a value that can't be erase.\n");
}

template <class container_t, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_map_inserter_t<container_t, ctx_t>::get_subfields_impl(const ctx_t &ctx) {
    json_adapter_map_t res;
    for (typename keys_set_t::iterator it =  added_keys.begin();
                                       it != added_keys.end();
                                       it++) {
        scoped_cJSON_t key(render_as_json(&*it, ctx));
        res[get_string(key.get())] = boost::shared_ptr<json_adapter_if_t<ctx_t> >(new json_adapter_t<typename container_t::mapped_type, ctx_t>(&(target->find(*it)->second)));
    }
    return res;
}

template <class container_t, class ctx_t>
boost::shared_ptr<subfield_change_functor_t<ctx_t> > json_map_inserter_t<container_t, ctx_t>::get_change_callback() {
    return boost::shared_ptr<subfield_change_functor_t<ctx_t> >(new standard_subfield_change_functor_t<container_t, ctx_t>(target));
}

//implementation for json_adapter_with_inserter_t
template <class container_t, class ctx_t>
json_adapter_with_inserter_t<container_t, ctx_t>::json_adapter_with_inserter_t(container_t *_target, gen_function_t _generator, value_t _initial_value, std::string _inserter_key)
    : target(_target), generator(_generator), 
      initial_value(_initial_value), inserter_key(_inserter_key)
{ }

template <class container_t, class ctx_t>
cJSON *json_adapter_with_inserter_t<container_t, ctx_t>::render_impl(const ctx_t &ctx) {
    return render_as_json(target, ctx);
}

template <class container_t, class ctx_t>
void json_adapter_with_inserter_t<container_t, ctx_t>::apply_impl(cJSON *change, const ctx_t &ctx) {
    apply_json_to(change, target, ctx);
}

template <class container_t, class ctx_t>
void json_adapter_with_inserter_t<container_t, ctx_t>::erase_impl(const ctx_t &ctx) {
    erase(target, ctx);
}

template <class container_t, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_with_inserter_t<container_t, ctx_t>::get_subfields_impl(const ctx_t &ctx) {
    json_adapter_map_t res = get_json_subfields(target, ctx);
    rassert(res.find(inserter_key) == res.end(), "Error, inserter_key %s  conflicts with another field of the target, (you probably want to change the value of inserter_key).", inserter_key.c_str());
    res[inserter_key] = boost::shared_ptr<json_adapter_if_t<ctx_t> >(new json_map_inserter_t<container_t, ctx_t>(target, generator, initial_value));

    return res;
}

template <class container_t, class ctx_t>
void json_adapter_with_inserter_t<container_t, ctx_t>::on_change(const ctx_t &ctx) {
    on_subfield_change(target, ctx);
}

template <class container_t, class ctx_t>
boost::shared_ptr<subfield_change_functor_t<ctx_t> > json_adapter_with_inserter_t<container_t, ctx_t>::get_change_callback() {
    return boost::shared_ptr<subfield_change_functor_t<ctx_t> >(new standard_subfield_change_functor_t<container_t, ctx_t>(target));
}

/* Here we have implementations of the json adapter concept for several
 * prominent types, these could in theory be relocated to a different file if
 * need be */

//JSON adapter for int
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(int *, const ctx_t &) {
    return typename json_adapter_if_t<ctx_t>::json_adapter_map_t();
}

template <class ctx_t>
cJSON *render_as_json(int *target, const ctx_t &) {
    return cJSON_CreateNumber(*target);
}

template <class ctx_t>
void apply_json_to(cJSON *change, int *target, const ctx_t &) {
    *target = get_int(change);
}

template <class ctx_t>
void on_subfield_change(int *, const ctx_t &) { }

//JSON adapter for char
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(char *, const ctx_t &) {
    return typename json_adapter_if_t<ctx_t>::json_adapter_map_t();
}

template <class ctx_t>
cJSON *render_as_json(char *target, const ctx_t &) {
    return cJSON_CreateString(target);
}

template <class ctx_t>
void apply_json_to(cJSON *change, char *target, const ctx_t &) {
    std::string str = get_string(change);
    if (str.size() != 1) {
        throw schema_mismatch_exc_t(strprintf("Trying to write %s to a char." 
                                    "The change should only be one character long.", str.c_str()));
    } else {
        *target = str[0];
    }
}

template <class ctx_t>
void on_subfield_change(char *, const ctx_t &) { }

namespace boost {

namespace uuids {
//JSON adapter for boost::uuids::uuid
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(boost::uuids::uuid *, const ctx_t &) {
    return std::map<std::string, boost::shared_ptr<json_adapter_if_t<ctx_t> > >();
}

template <class ctx_t>
cJSON *render_as_json(const boost::uuids::uuid *uuid, const ctx_t &) {
    if (uuid->is_nil()) {
        return cJSON_CreateNull();
    } else {
        return cJSON_CreateString(uuid_to_str(*uuid).c_str());
    }
}

template <class ctx_t>
void apply_json_to(cJSON *change, boost::uuids::uuid *uuid, const ctx_t &) {
    if (change->type == cJSON_NULL) {
        *uuid = boost::uuids::nil_generator()();
    } else {
        try {
            *uuid = str_to_uuid(get_string(change));
        } catch (std::runtime_error) {
            throw schema_mismatch_exc_t(strprintf("String %s, did not parse as uuid\n", get_string(change).c_str()));
        }
    }
}

template <class ctx_t>
void on_subfield_change(boost::uuids::uuid *, const ctx_t &) { }
} //namespace uuids 

//JSON adapter for boost::optional
template <class T, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(boost::optional<T> *target, const ctx_t &ctx) {
    if (*target) {
        return get_json_subfields(&**target, ctx);
    } else {
        return typename json_adapter_if_t<ctx_t>::json_adapter_map_t();
    }
}

template <class T, class ctx_t>
cJSON *render_as_json(boost::optional<T> *target, const ctx_t &ctx) {
    if (*target) {
        return render_as_json(&**target, ctx);
    } else {
        return cJSON_CreateString("Unset value");
    }
}

template <class T, class ctx_t>
void apply_json_to(cJSON *change, boost::optional<T> *target, const ctx_t &ctx) {
    if (!*target) {
        *target = T();
    }
    apply_json_to(change, &**target, ctx);
}

template <class T, class ctx_t>
void on_subfield_change(boost::optional<T> *, const ctx_t &) { }
} //namespace boost

namespace std {
//JSON adapter for std::string
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::string *, const ctx_t &) {
    return std::map<std::string, boost::shared_ptr<json_adapter_if_t<ctx_t> > >();
}

template <class ctx_t>
cJSON *render_as_json(std::string *str, const ctx_t &) {
    return cJSON_CreateString(str->c_str());
}

template <class ctx_t>
void apply_json_to(cJSON *change, std::string *str, const ctx_t &) {
    *str = get_string(change);
}

template <class ctx_t>
void on_subfield_change(std::string *, const ctx_t &) { }

//JSON adapter for std::map
template <class K, class V, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::map<K, V> *map, const ctx_t &ctx) {
    typename json_adapter_if_t<ctx_t>::json_adapter_map_t res;

#ifdef JSON_SHORTCUTS
    int shortcut_index = 0;
#endif

    for (typename std::map<K, V>::iterator it  = map->begin(); it != map->end(); ++it) {
        typename std::map<K, V>::key_type key = it->first;
        try {
            res[get_string(render_as_json(&key, ctx))] = boost::shared_ptr<json_adapter_if_t<ctx_t> >(new json_adapter_t<V, ctx_t>(&(it->second)));
        } catch (schema_mismatch_exc_t &) {
            crash("Someone tried to json adapt a std::map with a key type that"
                   "does not yield a JSON object of string type when"
                   "render_as_json is applied to it.");
        }

#ifdef JSON_SHORTCUTS
        res[strprintf("%d", shortcut_index)] = boost::shared_ptr<json_adapter_if_t<ctx_t> >(new json_adapter_t<V, ctx_t>(&(it->second)));
        shortcut_index++;
#endif
    }

    return res;
}

template <class K, class V, class ctx_t>
cJSON *render_as_json(std::map<K, V> *map, const ctx_t &ctx) {
    return render_as_directory(map, ctx);
}

template <class K, class V, class ctx_t>
void apply_json_to(cJSON *change, std::map<K, V> *map, const ctx_t &ctx) {
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;

    json_adapter_map_t elements = get_json_subfields(map, ctx);

    json_object_iterator_t it = get_object_it(change);

    cJSON *val;
    while ((val = it.next())) {
        if (std_contains(elements, val->string)) {
            elements[val->string]->apply(val, ctx);
        } else {
            K k;
            scoped_cJSON_t key(cJSON_CreateString(val->string));
            apply_json_to(key.get(), &k, ctx);

            V v;
            apply_json_to(val, &v, ctx);

            (*map)[k] = v;
        }
    }
}

template <class K, class V, class ctx_t>
void on_subfield_change(std::map<K, V> *, const ctx_t &) { }

//JSON adapter for std::set
template <class V, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::set<V> *, const ctx_t &) {
    return typename json_adapter_if_t<ctx_t>::json_adapter_map_t();
}

template <class V, class ctx_t>
cJSON *render_as_json(std::set<V> *target, const ctx_t &ctx) {
    cJSON *res = cJSON_CreateArray();

    for (typename std::set<V>::const_iterator it =  target->begin();
                                              it != target->end();
                                              it++) {
        V tmp = *it;
        cJSON_AddItemToArray(res, render_as_json(&tmp, ctx));
    }
    return res;
}

template <class V, class ctx_t>
void apply_json_to(cJSON *change, std::set<V> *target, const ctx_t &ctx) {
    std::set<V> res;
    json_array_iterator_t it = get_array_it(change);
    cJSON *val;
    while ((val = it.next())) {
        V v;
        apply_json_to(val, &v, ctx);
        res.insert(v);
    }

    *target = res;
}

template <class V, class ctx_t>
void on_subfield_change(std::set<V> *, const ctx_t &) { }
} //namespace std

//some convenience functions
template <class T, class ctx_t>
cJSON *render_as_directory(T *target, const ctx_t &ctx) {
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;

    cJSON *res = cJSON_CreateObject();

    json_adapter_map_t elements = get_json_subfields(target, ctx);
    for (typename json_adapter_map_t::iterator it =  elements.begin();
                                               it != elements.end();
                                               it++) {
        cJSON_AddItemToObject(res, it->first.c_str(), it->second->render(ctx));
    }

    return res;
}

template <class T, class ctx_t>
void apply_as_directory(cJSON *change, T *target, const ctx_t &ctx) {
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;
    json_adapter_map_t elements = get_json_subfields(target, ctx);

    json_object_iterator_t it = get_object_it(change);
    cJSON *val;
    while ((val = it.next())) {
        if (elements.find(val->string) == elements.end()) {
#ifndef NDEBUG
            logERR("Error, couldn't find element %s in adapter map.\n", val->string);
#else
            throw schema_mismatch_exc_t(strprintf("Couldn't find element %s.", val->string));
#endif
        } else {
            elements[val->string]->apply(val, ctx);
        }
    }
}

#endif
