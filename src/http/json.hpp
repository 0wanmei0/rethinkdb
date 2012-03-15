#ifndef __HTTP_JSON_HPP__
#define __HTTP_JSON_HPP__

#include <string>
#include <set>

#include "errors.hpp"
#include "http/json/cJSON.hpp"

class scoped_cJSON_t {
private:
    cJSON *val;

public:
    explicit scoped_cJSON_t(cJSON *);
    ~scoped_cJSON_t();
    cJSON *get() const;
    cJSON *release();
};

class json_iterator_t {
public:
    explicit json_iterator_t(cJSON *target);

    cJSON *next();
private:
    cJSON *node;
};

class json_object_iterator_t : public json_iterator_t {
public:
    explicit json_object_iterator_t(cJSON *target);
};

class json_array_iterator_t : public json_iterator_t {
public:
    explicit json_array_iterator_t(cJSON *target);
};

std::string cJSON_print_std_string(cJSON *json);

void project(cJSON *json, std::set<std::string> keys);

#endif
