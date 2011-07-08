#ifndef __RPC_SERIALIZE_BASIC_HPP__
#define __RPC_SERIALIZE_BASIC_HPP__

#include "rpc/serialize/serialize.hpp"

/* BEWARE: This file contains evil Boost witchery that depends on SFINAE and boost::type_traits
and all sorts of other bullshit. Here are some crosses to prevent the demonic witchery from
affecting the rest of the codebase:

    *                   *
    |                  *+*
*---X---*          *----+----*
    |                   |
    |                   |
    *                   *

*/

#include <boost/utility.hpp>
#include <boost/type_traits.hpp>

/* serialize() and unserialize() implementations for built-in arithmetic types */

// TODO! Don't need these anymore
/*
template<class T>
void serialize(cluster_outpipe_t *pipe, const T &value,
        typename boost::enable_if< boost::is_arithmetic<T> >::type * = 0) {
    pipe->write(&value, sizeof(value));
}

template<class T>
void unserialize(cluster_inpipe_t *pipe, UNUSED unserialize_extra_storage_t *es, T *value,
        typename boost::enable_if< boost::is_arithmetic<T> >::type * = 0) {
    pipe->read(value, sizeof(*value));
}*/

/* serialize() and unserialize() implementations for enums */

// TODO! Don't need these anymore
/*
template<class T>
void serialize(cluster_outpipe_t *pipe, const T &value,
        typename boost::enable_if< boost::is_enum<T> >::type * = 0) {
    pipe->write(&value, sizeof(value));
}

template<class T>
void unserialize(cluster_inpipe_t *pipe, UNUSED unserialize_extra_storage_t *es, T *value,
        typename boost::enable_if< boost::is_enum<T> >::type * = 0) {
    pipe->read(value, sizeof(*value));
}*/

#endif /* __RPC_SERIALIZE_BASIC_HPP__ */
