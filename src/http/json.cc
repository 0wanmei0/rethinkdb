#include <set>
#include <stdlib.h>
#include <vector>

#include <string.h>


#include "http/json.hpp"
#include "stl_utils.hpp"
#include "utils.hpp"
#include "containers/archive/stl_types.hpp"

scoped_cJSON_t::scoped_cJSON_t(cJSON *_val)
    : val(_val)
{ }

scoped_cJSON_t::~scoped_cJSON_t() {
    if (val) {
        cJSON_Delete(val);
    }
}

cJSON *scoped_cJSON_t::get() const {
    return val;
}

cJSON *scoped_cJSON_t::release() {
    cJSON *tmp = val;
    val = NULL;
    return tmp;
}

void scoped_cJSON_t::reset(cJSON *v) {
    if (val) {
        cJSON_Delete(val);
    }
    val = v;
}

copyable_cJSON_t::copyable_cJSON_t(cJSON *_val)
    : val(_val)
{ }

copyable_cJSON_t::copyable_cJSON_t(const copyable_cJSON_t &other) 
    : val(cJSON_DeepCopy(other.val))
{ }

copyable_cJSON_t::~copyable_cJSON_t() {
    cJSON_Delete(val);
}

cJSON *copyable_cJSON_t::get() const {
    return val;
}

write_message_t &copyable_cJSON_t::operator<<(write_message_t &msg) {
}
    MUST_USE archive_result_t deserialize(read_stream_t *s);

json_iterator_t::json_iterator_t(cJSON *target) {
    node = target->child;
}

cJSON *json_iterator_t::next() {
    cJSON *res = node;
    if (node) {
        node = node->next;
    }
    return res;
}

json_object_iterator_t::json_object_iterator_t(cJSON *target)
    : json_iterator_t(target)
{
    rassert(target->type == cJSON_Object);
}

json_array_iterator_t::json_array_iterator_t(cJSON *target)
    : json_iterator_t(target)
{
    rassert(target->type == cJSON_Array);
}

std::string cJSON_print_std_string(cJSON *json) {
    char *s = cJSON_Print(json);
    std::string res(s);
    free(s);

    return res;
}

std::string cJSON_print_unformatted_std_string(cJSON *json) {
    char *s = cJSON_PrintUnformatted(json);
    std::string res(s);
    free(s);

    return res;
}

void project(cJSON *json, std::set<std::string> keys) {
    rassert(json->type == cJSON_Object);

    json_object_iterator_t it(json);

    std::vector<std::string> keys_to_delete;

    while (cJSON *node = it.next()) {
        std::string str(node->string);
        if (!std_contains(keys, str)) {
            keys_to_delete.push_back(str);
        }
    }

    for (std::vector<std::string>::iterator it  = keys_to_delete.begin();
                                            it != keys_to_delete.end();
                                            ++it) {
        cJSON_DeleteItemFromObject(json, it->c_str());
    }
}

cJSON *merge(cJSON *x, cJSON *y) {
    cJSON *res = cJSON_CreateObject();
    json_object_iterator_t xit(x), yit(y);

    std::set<std::string> keys;
    cJSON *hd;

    while ((hd = xit.next())) {
        keys.insert(hd->string);
    }

    for (std::set<std::string>::iterator it = keys.begin();
                                         it != keys.end();
                                         ++it) {
        cJSON_AddItemToObject(res, it->c_str(), cJSON_DetachItemFromObject(x, it->c_str()));
    }

    keys.clear();

    while ((hd = yit.next())) {
        keys.insert(hd->string);
    }

    for (std::set<std::string>::iterator it = keys.begin();
                                         it != keys.end();
                                         ++it) {
        rassert(!cJSON_GetObjectItem(res, it->c_str()), "Overlapping names in merge, name was: %s\n", it->c_str());
        cJSON_AddItemToObject(res, it->c_str(), cJSON_DetachItemFromObject(y, it->c_str()));
    }

    return res;
}

write_message_t &operator<<(write_message_t &msg, const cJSON &cjson) {
    msg << cjson.type;

    switch(cjson.type) {
    case cJSON_False:
        break;
    case cJSON_True:
        break;
    case cJSON_NULL:
        break;
    case cJSON_Number:
        msg << cjson.valuedouble;
        break;
    case cJSON_String:
        {
            std::string s(cjson.valuestring);
            msg << s;
        }
        break;
    case cJSON_Array:
    case cJSON_Object:
        {
            msg << cJSON_GetArraySize(&cjson);

            cJSON *hd = cjson.child;
            while (hd) { 
                msg << *hd;
                hd = hd->next; 
            }
        }
        break;
    default:
        crash("Unreachable");
        break;
    }
    return msg;
}

#define CHECK_RES(res) if (res != ARCHIVE_SUCCESS) {\
                           return res;\
                       }


MUST_USE archive_result_t deserialize(read_stream_t *s, cJSON *cjson) {
    archive_result_t res = deserialize(s, &cjson->type);
    CHECK_RES(res);
    
    switch (cjson->type) {
    case cJSON_False:
    case cJSON_True:
    case cJSON_NULL:
        return ARCHIVE_SUCCESS;
        break;
    case cJSON_Number:
        res = deserialize(s, &cjson->valuedouble);
        CHECK_RES(res);
        cjson->valueint = int(cjson->valuedouble);
        return ARCHIVE_SUCCESS;
        break;
    case cJSON_String:
        {
            std::string str;
            res = deserialize(s, &str);
            CHECK_RES(res);
            cjson->valuestring = strdup(str.c_str());
            return ARCHIVE_SUCCESS;
        }
        break;
    case cJSON_Array:
        {
            int size;
            res = deserialize(s, &size);
            CHECK_RES(res);
            for (int i = 0; i < size; ++i) {
                cJSON *item = cJSON_CreateBlank();
                res = deserialize(s, item);
                CHECK_RES(res);
                cJSON_AddItemToArray(cjson, item);
            }
            return ARCHIVE_SUCCESS;
        }
        break;
    case cJSON_Object:
        {
            int size;
            res = deserialize(s, &size);
            CHECK_RES(res);
            for (int i = 0; i < size; ++i) {
                cJSON *item = cJSON_CreateBlank();
                res = deserialize(s, item);
                CHECK_RES(res);
                cJSON_AddItemToObject(cjson, item->string, item);
            }
            return ARCHIVE_SUCCESS;
        }
        break;
    default:
        crash("Unreachable");
        break;
    }
}
