#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <librdkafka/rdkafka.h>
#include <tarantool/module.h>

#include <common.h>
#include <callbacks.h>
#include <queue.h>

#include "producer.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Producer poll thread
 */

void *
producer_poll_loop(void *arg) {
    producer_poller_t *poller = arg;
    int count = 0;
    int should_stop = 0;

    while (true) {
        {
            pthread_mutex_lock(&poller->lock);

            should_stop = poller->should_stop;

            pthread_mutex_unlock(&poller->lock);

            if (should_stop) {
                break;
            }
        }

        {
            count = rd_kafka_poll(poller->rd_producer, 1000);
            if (count == 0) {
                // throttling calls with 100ms sleep
                usleep(100000);
            }
        }
    }

    pthread_exit(NULL);
}

producer_poller_t *
new_producer_poller(rd_kafka_t *rd_producer) {
    producer_poller_t *poller = NULL;
    poller = malloc(sizeof(producer_poller_t));
    if (poller == NULL) {
        return NULL;
    }
    poller->rd_producer = rd_producer;
    poller->should_stop = 0;

    pthread_mutex_init(&poller->lock, NULL);
    pthread_attr_init(&poller->attr);
    pthread_attr_setdetachstate(&poller->attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&poller->thread, &poller->attr, producer_poll_loop, (void *)poller);

    return poller;
}

static ssize_t
stop_poller(va_list args) {
    producer_poller_t *poller = va_arg(args, producer_poller_t *);
    pthread_mutex_lock(&poller->lock);

    poller->should_stop = 1;

    pthread_mutex_unlock(&poller->lock);

    pthread_join(poller->thread, NULL);

    return 0;
}

void
destroy_producer_poller(producer_poller_t *poller) {
    // stopping polling thread
    coio_call(stop_poller, poller);

    pthread_attr_destroy(&poller->attr);
    pthread_mutex_destroy(&poller->lock);

    free(poller);
}

/**
 * Producer
 */

producer_topics_t *
new_producer_topics(int32_t capacity) {
    rd_kafka_topic_t **elements;
    elements = malloc(sizeof(rd_kafka_topic_t *) * capacity);

    producer_topics_t *topics;
    topics = malloc(sizeof(producer_topics_t));
    topics->capacity = capacity;
    topics->count = 0;
    topics->elements = elements;

    return topics;
}

int
add_producer_topics(producer_topics_t *topics, rd_kafka_topic_t *element) {
    if (topics->count >= topics->capacity) {
        rd_kafka_topic_t **new_elements = realloc(topics->elements, sizeof(rd_kafka_topic_t *) * topics->capacity * 2);
        if (new_elements == NULL) {
            printf("realloc failed to relloc rd_kafka_topic_t array.");
            return 1;
        }
        topics->elements = new_elements;
        topics->capacity *= 2;
    }
    topics->elements[topics->count++] = element;
    return 0;
}

rd_kafka_topic_t *
find_producer_topic_by_name(producer_topics_t *topics, const char *name) {
    rd_kafka_topic_t *topic;
    for (int i = 0; i < topics->count; i++) {
        topic = topics->elements[i];
        if (strcmp(rd_kafka_topic_name(topic), name) == 0) {
            return topic;
        }
    }
    return NULL;
}

void
destroy_producer_topics(producer_topics_t *topics) {
    rd_kafka_topic_t **topic_p;
    rd_kafka_topic_t **end = topics->elements + topics->count;
    for (topic_p = topics->elements; topic_p < end; topic_p++) {
        rd_kafka_topic_destroy(*topic_p);
    }

    free(topics->elements);
    free(topics);
}

static inline producer_t *
lua_check_producer(struct lua_State *L, int index) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, index, producer_label);
    if (producer_p == NULL || *producer_p == NULL)
        luaL_error(L, "Kafka producer fatal error: failed to retrieve producer from lua stack!");
    return *producer_p;
}

int
lua_producer_tostring(struct lua_State *L) {
    producer_t *producer = lua_check_producer(L, 1);
    lua_pushfstring(L, "Kafka Producer: %p", producer);
    return 1;
}

int
lua_producer_msg_delivery_poll(struct lua_State *L) {
    if (lua_gettop(L) != 2)
        luaL_error(L, "Usage: count, err = producer:msg_delivery_poll(events_limit)");

    producer_t *producer = lua_check_producer(L, 1);

    int events_limit = lua_tonumber(L, 2);
    int callbacks_count = 0;
    char *err_str = NULL;
    dr_msg_t *dr_msg = NULL;

    pthread_mutex_lock(&producer->event_queues->delivery_queue->lock);

    while (events_limit > callbacks_count) {
        dr_msg = queue_lockfree_pop(producer->event_queues->delivery_queue);
        if (dr_msg == NULL) {
            break;
        }
        callbacks_count += 1;
        lua_rawgeti(L, LUA_REGISTRYINDEX, dr_msg->dr_callback);
        if (dr_msg->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            lua_pushstring(L, (char *)rd_kafka_err2str(dr_msg->err));
        } else {
            lua_pushnil(L);
        }
        /* do the call (1 arguments, 0 result) */
        if (lua_pcall(L, 1, 0, 0) != 0) {
            err_str = (char *)lua_tostring(L, -1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, dr_msg->dr_callback);
        destroy_dr_msg(dr_msg);
        if (err_str != NULL) {
            break;
        }
    }

    pthread_mutex_unlock(&producer->event_queues->delivery_queue->lock);

    lua_pushnumber(L, (double)callbacks_count);
    if (err_str != NULL) {
        int fail = safe_pushstring(L, err_str);
        if (fail) {
            return lua_push_error(L);
        }
    } else {
        lua_pushnil(L);
    }
    return 2;
}

int
lua_producer_poll_logs(struct lua_State *L) {
    if (lua_gettop(L) != 2)
        luaL_error(L, "Usage: count, err = producer:poll_logs(limit)");

    producer_t *producer = lua_check_producer(L, 1);
    if (producer->event_queues == NULL || producer->event_queues->log_queue == NULL || producer->event_queues->log_cb_ref == LUA_REFNIL) {
        lua_pushnumber(L, 0);
        int fail = safe_pushstring(L, "Producer poll logs error: callback for logs is not set");
        if (fail) {
            return lua_push_error(L);
        }
        return 2;
    }

    int limit = lua_tonumber(L, 2);
    log_msg_t *msg = NULL;
    int count = 0;
    char *err_str = NULL;
    while (count < limit) {
        msg = queue_pop(producer->event_queues->log_queue) ;
        if (msg == NULL) {
            break;
        }
        count++;
        lua_rawgeti(L, LUA_REGISTRYINDEX, producer->event_queues->log_cb_ref);
        lua_pushstring(L, msg->fac);
        lua_pushstring(L, msg->buf);
        lua_pushnumber(L, (double)msg->level);
        /* do the call (3 arguments, 0 result) */
        if (lua_pcall(L, 3, 0, 0) != 0) {
            err_str = (char *)lua_tostring(L, -1);
        }

        destroy_log_msg(msg);

        if (err_str != NULL) {
            break;
        }
    }
    lua_pushnumber(L, (double)count);
    if (err_str != NULL) {
        int fail = safe_pushstring(L, err_str);
        if (fail) {
            return lua_push_error(L);
        }
    } else {
        lua_pushnil(L);
    }
    return 2;
}

int
lua_producer_poll_errors(struct lua_State *L) {
    if (lua_gettop(L) != 2)
        luaL_error(L, "Usage: count, err = producer:poll_errors(limit)");

    producer_t *producer = lua_check_producer(L, 1);
    if (producer->event_queues == NULL || producer->event_queues->error_queue == NULL || producer->event_queues->error_cb_ref == LUA_REFNIL) {
        lua_pushnumber(L, 0);
        int fail = safe_pushstring(L, "Producer poll errors error: callback for logs is not set");
        if (fail) {
            return lua_push_error(L);
        }
        return 2;
    }

    int limit = lua_tonumber(L, 2);
    error_msg_t *msg = NULL;
    int count = 0;
    char *err_str = NULL;
    while (count < limit) {
        msg = queue_pop(producer->event_queues->error_queue) ;
        if (msg == NULL) {
            break;
        }
        count++;
        lua_rawgeti(L, LUA_REGISTRYINDEX, producer->event_queues->error_cb_ref);
        lua_pushstring(L, msg->reason);
        /* do the call (1 arguments, 0 result) */
        if (lua_pcall(L, 1, 0, 0) != 0) {
            err_str = (char *)lua_tostring(L, -1);
        }

        destroy_error_msg(msg);

        if (err_str != NULL) {
            break;
        }
    }
    lua_pushnumber(L, (double)count);
    if (err_str != NULL) {
        int fail = safe_pushstring(L, err_str);
        if (fail) {
            return lua_push_error(L);
        }
    } else {
        lua_pushnil(L);
    }
    return 2;
}

int
lua_producer_produce(struct lua_State *L) {
    if (lua_gettop(L) != 2 || !lua_istable(L, 2))
        luaL_error(L, "Usage: err = producer:produce(msg)");

    lua_pushstring(L, "topic");
    lua_gettable(L, -2 );
    const char *topic = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (topic == NULL) {
        int fail = safe_pushstring(L, "producer message must contains non nil 'topic' key");
        return fail ? lua_push_error(L): 1;
    }

    lua_pushstring(L, "key");
    lua_gettable(L, -2 );
    size_t key_len;
    // rd_kafka will copy key so no need to worry about this cast
    char *key = (char *)lua_tolstring(L, -1, &key_len);

    lua_pop(L, 1);

    lua_pushstring(L, "value");
    lua_gettable(L, -2 );
    size_t value_len;
    // rd_kafka will copy value so no need to worry about this cast
    char *value = (char *)lua_tolstring(L, -1, &value_len);

    lua_pop(L, 1);

    if (key == NULL && value == NULL) {
        int fail = safe_pushstring(L, "producer message must contains non nil key or value");
        return fail ? lua_push_error(L): 1;
    }

    // create delivery callback queue if got msg id
    dr_msg_t *dr_msg = NULL;
    lua_pushstring(L, "dr_callback");
    lua_gettable(L, -2 );
    if (lua_isfunction(L, -1)) {
        dr_msg = new_dr_msg(luaL_ref(L, LUA_REGISTRYINDEX), RD_KAFKA_RESP_ERR_NO_ERROR);
        if (dr_msg == NULL) {
            int fail = safe_pushstring(L, "failed to create callback message");
            return fail ? lua_push_error(L): 1;
        }
    } else {
        lua_pop(L, 1);
    }

    // pop msg
    lua_pop(L, 1);

    producer_t *producer = lua_check_producer(L, 1);
    rd_kafka_topic_t *rd_topic = find_producer_topic_by_name(producer->topics, topic);
    if (rd_topic == NULL) {
        rd_topic = rd_kafka_topic_new(producer->rd_producer, topic, NULL);
        if (rd_topic == NULL) {
            const char *const_err_str = rd_kafka_err2str(rd_kafka_errno2err(errno));
            char err_str[512];
            strcpy(err_str, const_err_str);
            int fail = safe_pushstring(L, err_str);
            return fail ? lua_push_error(L): 1;
        }
        if (add_producer_topics(producer->topics, rd_topic) != 0) {
            int fail = safe_pushstring(L, "Unexpected error: failed to add new topic to topic list!");
            return fail ? lua_push_error(L): 1;
        }
    }

    if (rd_kafka_produce(rd_topic, -1, RD_KAFKA_MSG_F_COPY, value, value_len, key, key_len, dr_msg) == -1) {
        const char *const_err_str = rd_kafka_err2str(rd_kafka_errno2err(errno));
        char err_str[512];
        strcpy(err_str, const_err_str);
        int fail = safe_pushstring(L, err_str);
        return fail ? lua_push_error(L): 1;
    }
    return 0;
}

static ssize_t
producer_flush(va_list args) {
    rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
    rd_kafka_t *rd_producer = va_arg(args, rd_kafka_t *);
    while (true) {
        err = rd_kafka_flush(rd_producer, 1000);
        if (err != RD_KAFKA_RESP_ERR__TIMED_OUT) {
            break;
        }
    }
    return 0;
}

static ssize_t
wait_producer_destroy(va_list args) {
    rd_kafka_t *rd_kafka = va_arg(args, rd_kafka_t *);
    rd_kafka_destroy(rd_kafka);
    return 0;
}

void
destroy_producer(struct lua_State *L, producer_t *producer) {
    if (producer->rd_producer != NULL) {
        coio_call(producer_flush, producer->rd_producer);
    }

    if (producer->poller != NULL) {
        destroy_producer_poller(producer->poller);
    }

    if (producer->topics != NULL) {
        destroy_producer_topics(producer->topics);
    }

    if (producer->event_queues != NULL) {
        destroy_event_queues(L, producer->event_queues);
    }

    if (producer->rd_producer != NULL) {
        /* Destroy handle */
        coio_call(wait_producer_destroy, producer->rd_producer);
    }

    free(producer);
}

int
lua_producer_close(struct lua_State *L) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, 1, producer_label);
    if (producer_p == NULL || *producer_p == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }

    if ((*producer_p)->rd_producer != NULL) {
        coio_call(producer_flush, (*producer_p)->rd_producer);
    }

    if ((*producer_p)->poller != NULL) {
        destroy_producer_poller((*producer_p)->poller);
        (*producer_p)->poller = NULL;
    }

    lua_pushboolean(L, 1);
    return 1;
}

int
lua_producer_destroy(struct lua_State *L) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, 1, producer_label);
    if (producer_p && *producer_p) {
        destroy_producer(L, *producer_p);
    }
    if (producer_p)
        *producer_p = NULL;
    return 0;
}

int
lua_create_producer(struct lua_State *L) {
    if (lua_gettop(L) != 1 || !lua_istable(L, 1))
        luaL_error(L, "Usage: producer, err = create_producer(conf)");

    lua_pushstring(L, "brokers");
    lua_gettable(L, -2 );
    const char *brokers = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (brokers == NULL) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, "producer config table must have non nil key 'brokers' which contains string");
        return fail ? lua_push_error(L): 2;
    }

    char errstr[512];

    rd_kafka_conf_t *rd_config = rd_kafka_conf_new();

    rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
    lua_pushstring(L, "default_topic_options");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        // stack now contains: -1 => nil; -2 => table
        while (lua_next(L, -2)) {
            // stack now contains: -1 => value; -2 => key; -3 => table
            if (!(lua_isstring(L, -1)) || !(lua_isstring(L, -2))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, "producer config default topic options must contains only string keys and string values");
                return fail ? lua_push_error(L): 2;
            }

            const char *value = lua_tostring(L, -1);
            const char *key = lua_tostring(L, -2);
            if (rd_kafka_topic_conf_set(topic_conf, key, value, errstr, sizeof(errstr))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, errstr);
                return fail ? lua_push_error(L): 2;
            }

            // pop value, leaving original key
            lua_pop(L, 1);
            // stack now contains: -1 => key; -2 => table
        }
        // stack now contains: -1 => table
    }
    lua_pop(L, 1);
    rd_kafka_conf_set_default_topic_conf(rd_config, topic_conf);

    event_queues_t *event_queues = new_event_queues();
    event_queues->delivery_queue = new_queue();
    rd_kafka_conf_set_dr_msg_cb(rd_config, msg_delivery_callback);

    lua_pushstring(L, "error_callback");
    lua_gettable(L, -2 );
    if (lua_isfunction(L, -1)) {
        event_queues->error_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        event_queues->error_queue = new_queue();
        rd_kafka_conf_set_error_cb(rd_config, error_callback);
    } else {
        lua_pop(L, 1);
    }

    lua_pushstring(L, "log_callback");
    lua_gettable(L, -2 );
    if (lua_isfunction(L, -1)) {
        event_queues->log_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        event_queues->log_queue = new_queue();
        rd_kafka_conf_set_log_cb(rd_config, log_callback);
    } else {
        lua_pop(L, 1);
    }

    rd_kafka_conf_set_opaque(rd_config, event_queues);

    lua_pushstring(L, "options");
    lua_gettable(L, -2 );
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        // stack now contains: -1 => nil; -2 => table
        while (lua_next(L, -2)) {
            // stack now contains: -1 => value; -2 => key; -3 => table
            if (!(lua_isstring(L, -1)) || !(lua_isstring(L, -2))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, "producer config options must contains only string keys and string values");
                return fail ? lua_push_error(L): 2;
            }

            const char *value = lua_tostring(L, -1);
            const char *key = lua_tostring(L, -2);
            if (rd_kafka_conf_set(rd_config, key, value, errstr, sizeof(errstr))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, errstr);
                return fail ? lua_push_error(L): 2;
            }

            // pop value, leaving original key
            lua_pop(L, 1);
            // stack now contains: -1 => key; -2 => table
        }
        // stack now contains: -1 => table
    }
    lua_pop(L, 1);

    rd_kafka_t *rd_producer;
    if (!(rd_producer = rd_kafka_new(RD_KAFKA_PRODUCER, rd_config, errstr, sizeof(errstr)))) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, errstr);
        return fail ? lua_push_error(L): 2;
    }

    if (rd_kafka_brokers_add(rd_producer, brokers) == 0) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, "No valid brokers specified");
        return fail ? lua_push_error(L): 2;
    }

    // creating background thread for polling consumer
    producer_poller_t *poller = new_producer_poller(rd_producer);

    producer_t *producer;
    producer = malloc(sizeof(producer_t));
    producer->rd_producer = rd_producer;
    producer->topics = new_producer_topics(256);
    producer->event_queues = event_queues;
    producer->poller = poller;

    producer_t **producer_p = (producer_t **)lua_newuserdata(L, sizeof(producer));
    *producer_p = producer;

    luaL_getmetatable(L, producer_label);
    lua_setmetatable(L, -2);
    return 1;
}
