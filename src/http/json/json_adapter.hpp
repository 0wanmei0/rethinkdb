#ifndef HTTP_JSON_JSON_ADAPTER_HPP_
#define HTTP_JSON_JSON_ADAPTER_HPP_

#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "errors.hpp"
#include <boost/function.hpp>
#include <boost/optional/optional.hpp>
#include <boost/ptr_container/ptr_list.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_serialize.hpp>

#include "http/json.hpp"

struct json_adapter_exc_t : public std::exception { 
    virtual const char *what() const throw () {
        return "Generic json adapter exception\n";
    }

    virtual ~json_adapter_exc_t() throw () { }
};

struct schema_mismatch_exc_t : public json_adapter_exc_t {
private:
    std::string desc;
public:
    explicit schema_mismatch_exc_t(const std::string &_desc)
        : desc(_desc)
    { }

    const char *what() const throw () {
        return desc.c_str();
    }

    ~schema_mismatch_exc_t() throw () { }
};

struct permission_denied_exc_t : public json_adapter_exc_t {
private:
    std::string desc;
public:
    explicit permission_denied_exc_t(const std::string &_desc)
        : desc(_desc)
    { }

    const char *what() const throw () {
        return desc.c_str();
    }

    ~permission_denied_exc_t() throw () { }
};

struct multiple_choices_exc_t : public json_adapter_exc_t {
    virtual const char *what() const throw () {
        return "Multiple choices exists for this json value (probably vector clock divergence).";
    }
};

//Functions to make accessing cJSON *objects easier

bool get_bool(cJSON *json);

std::string get_string(cJSON *json);

int get_int(cJSON *json);

double get_double(cJSON *json);

json_array_iterator_t get_array_it(cJSON *json);

json_object_iterator_t get_object_it(cJSON *json);

template <class ctx_t>
class subfield_change_functor_t {
public:
    virtual void on_change(const ctx_t &) = 0;
    virtual ~subfield_change_functor_t() { }
};

template <class ctx_t>
class noop_subfield_change_functor_t : public subfield_change_functor_t<ctx_t> {
public:
    void on_change(const ctx_t &) { }
};

template <class T, class ctx_t>
class standard_subfield_change_functor_t : public subfield_change_functor_t<ctx_t>{
private:
    T *target;
public:
    explicit standard_subfield_change_functor_t(T *);
    void on_change(const ctx_t &);
};

//TODO come up with a better name for this
template <class ctx_t>
class json_adapter_if_t {
public:
    typedef std::map<std::string, boost::shared_ptr<json_adapter_if_t> > json_adapter_map_t;

private:
    virtual json_adapter_map_t get_subfields_impl(const ctx_t &) = 0;
    virtual cJSON *render_impl(const ctx_t &) = 0;
    virtual void apply_impl(cJSON *, const ctx_t &) = 0;
    virtual void erase_impl(const ctx_t &) = 0;
    /* follows the creation paradigm, ie the caller is responsible for the
     * object this points to */
    virtual boost::shared_ptr<subfield_change_functor_t<ctx_t> >  get_change_callback() = 0;

    std::vector<boost::shared_ptr<subfield_change_functor_t<ctx_t> > > superfields;

public:
    json_adapter_map_t get_subfields(const ctx_t &);
    cJSON *render(const ctx_t &);
    void apply(cJSON *, const ctx_t &);
    void erase(const ctx_t &);
    virtual ~json_adapter_if_t() { }
};

/* A json adapter is the most basic adapter, you can instantiate one with any
 * type that implements the json adapter concept as T */
template <class T, class ctx_t>
class json_adapter_t : public json_adapter_if_t<ctx_t> {
private:
    T *target;
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;
public:
    explicit json_adapter_t(T *);

private:
    json_adapter_map_t get_subfields_impl(const ctx_t &);
    cJSON *render_impl(const ctx_t &);
    virtual void apply_impl(cJSON *, const ctx_t &);
    virtual void erase_impl(const ctx_t &);
    boost::shared_ptr<subfield_change_functor_t<ctx_t> > get_change_callback();
};

/* A read only adapter is like a normal adapter but it throws an exception when
 * you try to do an apply call. */
template <class T, class ctx_t>
class json_read_only_adapter_t : public json_adapter_t<T, ctx_t> {
public:
    explicit json_read_only_adapter_t(T *);
private:
    void apply_impl(cJSON *, const ctx_t &);
    void erase_impl(const ctx_t &);
};

/* A json temporary adapter is like a read only adapter but it stores a copy of
 * the what it's adapting inside it. This is convenient when we want to have
 * json data that's not actually reflected in our structures such as having the
 * id of every element in a map referenced in an id field */
template <class T, class ctx_t>
class json_temporary_adapter_t : public json_read_only_adapter_t<T, ctx_t> {
private:
    T t;
public:
    explicit json_temporary_adapter_t(const T &);
};

/* This adapter is a little bit different from the other ones, it's meant to
 * target a map and allow a way to insert in to it, using serveside generated
 * keys. Because of this the render and apply functions have different schemas
 * in particular:
 *
 *  If target is of type map<K,V>
 *  schema(render) = schema(render_as_json(map<K,V>))
 *  schema(apply) = shchema(apply_json_to(V))
 *
 *  Rendering an inserter will give you only the entries in the map that were
 *  created using this inserter. Thus rendering with a newly constructed
 *  inserter gives you an empty map.
 */
template <class container_t, class ctx_t>
class json_map_inserter_t : public json_adapter_if_t<ctx_t> {
private:
    container_t *target;
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;

    typedef boost::function<typename container_t::key_type()> gen_function_t;
    gen_function_t generator;

    typedef typename container_t::mapped_type value_t;
    value_t initial_value;

    typedef std::set<typename container_t::key_type> keys_set_t;
    keys_set_t added_keys;

public:
    json_map_inserter_t(container_t *, gen_function_t, value_t _initial_value = value_t());

private:
    cJSON *render_impl(const ctx_t &);
    void apply_impl(cJSON *, const ctx_t &);
    void erase_impl(const ctx_t &);
    json_adapter_map_t get_subfields_impl(const ctx_t &);
    boost::shared_ptr<subfield_change_functor_t<ctx_t> > get_change_callback();
};

/* This combines the inserter json adapter with the standard adapter for a map,
 * thus creating an adapter for a map with which we can do normal modifications
 * and insertions */
template <class container_t, class ctx_t>
class json_adapter_with_inserter_t : public json_adapter_if_t<ctx_t> {
private:
    container_t *target;
    typedef typename json_adapter_if_t<ctx_t>::json_adapter_map_t json_adapter_map_t;

    typedef boost::function<typename container_t::key_type()> gen_function_t;
    gen_function_t generator;

    typedef typename container_t::mapped_type value_t;
    value_t initial_value;

    std::string inserter_key;

public:
    json_adapter_with_inserter_t(container_t *, gen_function_t, value_t _initial_value = value_t(), std::string _inserter_key = std::string("new"));

private:
    json_adapter_map_t get_subfields_impl(const ctx_t &);
    cJSON *render_impl(const ctx_t &);
    void apply_impl(cJSON *, const ctx_t &);
    void erase_impl(const ctx_t &);
    void on_change(const ctx_t &);
    boost::shared_ptr<subfield_change_functor_t<ctx_t> > get_change_callback();
};

/* Erase is a fairly rare function for an adapter to allow so we implement a
 * generic version of it. */
template <class T, class ctx_t>
void erase(T *, const ctx_t &) { 
#ifndef NDEBUG
    throw permission_denied_exc_t("Can't erase this object.. by default"
            "json_adapters disallow deletion. if you'd like to be able to please"
            "implement a working erase method for it.");
#else
    throw permission_denied_exc_t("Can't erase this object.");
#endif
}

/* Here we have implementations of the json adapter concept for several
 * prominent types, these could in theory be relocated to a different file if
 * need be */


//JSON adapter for int
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(int *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(int *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, int *, const ctx_t &);

template <class ctx_t>
void on_subfield_change(int *, const ctx_t &);

//JSON adapter for char
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(char *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(char *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, char *, const ctx_t &);

template <class ctx_t>
void on_subfield_change(char *, const ctx_t &);

namespace boost {
//JSON adapter for boost::uuids::uuid
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(boost::uuids::uuid *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(const boost::uuids::uuid *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, boost::uuids::uuid *, const ctx_t &);

template <class ctx_t>
void on_subfield_change(boost::uuids::uuid *, const ctx_t &);

//JSON adapter for boost::optional
template <class T, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(boost::optional<T> *, const ctx_t &);

template <class T, class ctx_t>
cJSON *render_as_json(boost::optional<T> *, const ctx_t &);

template <class T, class ctx_t>
void apply_json_to(cJSON *, boost::optional<T> *, const ctx_t &);

template <class T, class ctx_t>
void on_subfield_change(boost::optional<T> *, const ctx_t &);
} //namespace boost

namespace std {
//JSON adapter for std::string
template <class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::string *, const ctx_t &);

template <class ctx_t>
cJSON *render_as_json(std::string *, const ctx_t &);

template <class ctx_t>
void apply_json_to(cJSON *, std::string *, const ctx_t &);

template <class ctx_t>
void  on_subfield_change(std::string *, const ctx_t &);

//JSON adapter for std::map
template <class K, class V, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::map<K, V> *, const ctx_t &);

template <class K, class V, class ctx_t>
cJSON *render_as_json(std::map<K, V> *, const ctx_t &);

template <class K, class V, class ctx_t>
void apply_json_to(cJSON *, std::map<K, V> *, const ctx_t &);

template <class K, class V, class ctx_t>
void on_subfield_change(std::map<K, V> *, const ctx_t &);

//JSON adapter for std::set
template <class V, class ctx_t>
typename json_adapter_if_t<ctx_t>::json_adapter_map_t get_json_subfields(std::set<V> *, const ctx_t &);

template <class V, class ctx_t>
cJSON *render_as_json(std::set<V> *, const ctx_t &);

template <class V, class ctx_t>
void apply_json_to(cJSON *, std::set<V> *, const ctx_t &);

template <class V, class ctx_t>
void on_subfield_change(std::set<V> *, const ctx_t &);
} //namespace std

//some convenience functions
template <class T, class ctx_t>
cJSON *render_as_directory(T *, const ctx_t &);

template <class T, class ctx_t>
void apply_as_directory(cJSON *change, T *, const ctx_t &);

#include "http/json/json_adapter.tcc"

#endif

