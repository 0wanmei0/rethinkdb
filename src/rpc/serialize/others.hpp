#ifndef __RPC_SERIALIZE_OTHERS_HPP__
#define __RPC_SERIALIZE_OTHERS_HPP__

#include "rpc/serialize/serialize.hpp"
#include "rpc/serialize/basic.hpp"
#include "store.hpp"
#include <map>

/* Serializing and unserializing std::vector of a serializable type */

template<class element_t>
void serialize(cluster_outpipe_t *conn, const std::vector<element_t> &e) {
    serialize(conn, int(e.size()));
    for (int i = 0; i < (int)e.size(); i++) {
        serialize(conn, e[i]);
    }
}

template<class element_t>
int ser_size(const std::vector<element_t> &e) {
    int size = 0;
    size += ser_size(int(e.size()));
    for (int i = 0; i < (int)e.size(); i++) {
        size += ser_size(e[i]);
    }
    return size;
}

template<class element_t>
void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, std::vector<element_t> *e) {
    int count;
    unserialize(conn, es, &count);
    e->resize(count);
    for (int i = 0; i < count; i++) {
        element_t *ei = &(*e)[i];
        unserialize(conn, es, ei);
    }
}

/* Serializing and unserializing std::pair of 2 serializable types */

template<class T, class U>
void serialize(cluster_outpipe_t *conn, const std::pair<T, U> &pair) {
    serialize(conn, pair.first);
    serialize(conn, pair.second);
}

template<class T, class U>
int ser_size(const std::pair<T, U> &pair) {
    return ser_size(pair.first) + ser_size(pair.second);
}

template<class T, class U>
void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, std::pair<T, U> *pair) {
    unserialize(conn, es, &(pair->first));
    unserialize(conn, es, &(pair->first));
}

/* Serializing and unserializing std::map from a serializable type to another serializable type */
template<class K, class V>
void serialize(cluster_outpipe_t *conn, const std::map<K, V> &m) {
    serialize(conn, int(m.size()));
    for (typename std::map<K, V>::const_iterator it = m.begin(); it != m.end(); it++) {
        serialize(conn, it->first);
        serialize(conn, it->second);
    }
}

template<class K, class V>
int ser_size(const std::map<K, V> &m) {
    int size = 0;
    size += ser_size(int(m.size()));
    for (typename std::map<K, V>::const_iterator it = m.begin(); it != m.end(); it++) {
        size += ser_size((*it).first);
        size += ser_size((*it).second);
    }
    return size;
}

template<class K, class V>
void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, std::map<K, V> *m) {
    int count;
    unserialize(conn, es, &count);
    for (int i = 0; i < count; i++) {
        K key;
        unserialize(conn, es, &key);
        unserialize(conn, es, &((*m)[key]));
    }
}

/* Serializing and unserializing boost::scoped_ptr */

template<class object_t>
void serialize(cluster_outpipe_t *conn, const boost::scoped_ptr<object_t> &p) {
    if (p) {
        serialize(conn, true);
        serialize(conn, *p);
    } else {
        serialize(conn, false);
    }
}

template<class object_t>
int ser_size(const boost::scoped_ptr<object_t> &p) {
    if (p) {
        return ser_size(true) + ser_size(*p);
    } else {
        return ser_size(false);
    }
}

template<class object_t>
void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, boost::scoped_ptr<object_t> *p) {
    bool is_non_null;
    unserialize(conn, es, &is_non_null);
    if (is_non_null) {
        object_t *buffer = new object_t;
        p->reset(buffer);
        unserialize(conn, es, buffer);
    } else {
        p->reset(NULL);
    }
}

/* Serializing and unserializing ip_address_t */

inline void serialize(cluster_outpipe_t *conn, const ip_address_t &addr) {
    rassert(sizeof(ip_address_t) == 4);
    conn->write(&addr, sizeof(ip_address_t));
}

inline int ser_size(const ip_address_t *addr) {
    return sizeof(*addr);
}

inline void unserialize(cluster_inpipe_t *conn, UNUSED unserialize_extra_storage_t *es, ip_address_t *addr) {
    rassert(sizeof(ip_address_t) == 4);
    conn->read(addr, sizeof(ip_address_t));
}

/* Serializing and unserializing store_key_t */

inline void serialize(cluster_outpipe_t *conn, const store_key_t &key) {
    conn->write(&key.size, sizeof(key.size));
    conn->write(key.contents, key.size);
}

inline int ser_size(const store_key_t &key) {
    int size = 0;
    size += sizeof(key.size);
    size += key.size;
    return size;
}

inline void unserialize(cluster_inpipe_t *conn, UNUSED unserialize_extra_storage_t *es, store_key_t *key) {
    conn->read(&key->size, sizeof(key->size));
    conn->read(key->contents, key->size);
}

/* Serializing and unserializing data_provider_ts. Two flavors are provided: one that works with
boost::shared_ptr<data_provider_t>, and one that works with raw data_provider_t*. */

inline void serialize(cluster_outpipe_t *conn,  data_provider_t *data) {
    if (data) {
        ::serialize(conn, true);
        int size = data->get_size();
        ::serialize(conn, size);
        const const_buffer_group_t *buffers = data->get_data_as_buffers();
        for (int i = 0; i < (int)buffers->num_buffers(); i++) {
            conn->write(buffers->get_buffer(i).data, buffers->get_buffer(i).size);
        }
    } else {
        ::serialize(conn, false);
    }
}

inline int ser_size(data_provider_t *data) {
    if (data) {
        int size = data->get_size();
        return ser_size(true) + ser_size(size) + data->get_size();
    } else {
        return ser_size(false);
    }
}

inline void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, data_provider_t **data) {
    bool non_null;
    ::unserialize(conn, es, &non_null);
    if (non_null) {
        int size;
        ::unserialize(conn, es, &size);
        void *buffer;
        /* The buffered_data_provider_t needs to remain after we return, so we can't allocate it on
        the stack. It needs to be deleted eventually, so we can't just call "operator new" and
        forget about it. This is what unserialize_extra_storage_t is for: we call allocate() and
        it is guaranteed to be freed at the right time. */
        *data = es->reg(new buffered_data_provider_t(size, &buffer));
        conn->read(buffer, size);
    } else {
        *data = NULL;
    }
}

inline void serialize(cluster_outpipe_t *conn, const boost::shared_ptr<data_provider_t> &data) {
    serialize(conn, data.get());
}

inline int ser_size(const boost::shared_ptr<data_provider_t> &data) {
    return ser_size(data.get());
}

inline void unserialize(cluster_inpipe_t *conn, unserialize_extra_storage_t *es, boost::shared_ptr<data_provider_t> *data) {
    bool non_null;
    ::unserialize(conn, es, &non_null);
    if (non_null) {
        int size;
        ::unserialize(conn, es, &size);
        void *buffer;
        /* We don't need to use the unserialize_extra_storage_t because we have a smart pointer. */
        (*data).reset(new buffered_data_provider_t(size, &buffer));
        conn->read(buffer, size);
    } else {
        (*data).reset();
    }
}

/* Serializing and unserializing `order_token_t`. For now we don't actually serialize
anything because we don't have a way of making sure that buckets are unique across
different machines in the cluster. */

inline void serialize(UNUSED cluster_outpipe_t *conn, UNUSED const order_token_t &tok) {
    // Do nothing
}

inline int ser_size(UNUSED const order_token_t &tok) {
    return 0;
}

inline void unserialize(UNUSED cluster_inpipe_t *conn, UNUSED unserialize_extra_storage_t *es, order_token_t *tok) {
    *tok = order_token_t::ignore;
}

#endif /* __RPC_SERIALIZE_OTHERS_HPP__ */
