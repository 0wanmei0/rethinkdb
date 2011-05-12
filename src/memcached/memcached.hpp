#ifndef __MEMCACHED_MEMCACHED_HPP__
#define __MEMCACHED_MEMCACHED_HPP__

#include "arch/arch.hpp"

struct get_store_t;
struct set_store_interface_t;
class order_source_t;

void serve_memcache(tcp_conn_t *conn, get_store_t *get_store, set_store_interface_t *set_store, order_source_t *order_source);

#endif /* __MEMCACHED_MEMCACHED_HPP__ */
