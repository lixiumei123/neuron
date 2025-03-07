/**
 * NEURON IIoT System for Industry 4.0
 * Copyright (C) 2020-2022 EMQ Technologies Co., Ltd All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 **/
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/supplemental/util/platform.h>

#include "event/event.h"
#include "persist/persist.h"
#include "utils/log.h"
#include "utils/time.h"

#include "adapter.h"
#include "adapter/adapter_internal.h"
#include "adapter/driver/driver_internal.h"
#include "errcodes.h"

#include "node_manager.h"
#include "plugin_manager.h"
#include "storage.h"
#include "subscribe.h"

#include "manager.h"
#include "manager_internal.h"

// definition for adapter names
#define DEFAULT_DASHBOARD_ADAPTER_NAME DEFAULT_DASHBOARD_PLUGIN_NAME

static const char *const url = "inproc://neu_manager";

static int manager_loop(enum neu_event_io_type type, int fd, void *usr_data);
static int manager_level_check(void *usr_data);
inline static void reply(neu_manager_t *manager, neu_reqresp_head_t *header,
                         void *data);
inline static void forward_msg_dup(neu_manager_t *manager, nng_msg *msg,
                                   nng_pipe pipe);
inline static void forward_msg(neu_manager_t *manager, nng_msg *msg,
                               const char *node);
inline static void notify_monitor(neu_manager_t *    manager,
                                  neu_reqresp_type_e event, void *data);
static void start_static_adapter(neu_manager_t *manager, const char *name);
static int  update_timestamp(void *usr_data);
static void start_single_adapter(neu_manager_t *manager, const char *name,
                                 const char *plugin_name, bool display);

neu_manager_t *neu_manager_create()
{
    int                  rv      = 0;
    neu_manager_t *      manager = calloc(1, sizeof(neu_manager_t));
    neu_event_io_param_t param   = {
        .usr_data = (void *) manager,
        .cb       = manager_loop,
    };

    neu_event_timer_param_t timestamp_timer_param = {
        .second      = 0,
        .millisecond = 10,
        .cb          = update_timestamp,
        .type        = NEU_EVENT_TIMER_NOBLOCK,
    };

    neu_event_timer_param_t timer_level = {
        .second      = 60,
        .millisecond = 0,
        .cb          = manager_level_check,
        .type        = NEU_EVENT_TIMER_BLOCK,
    };

    manager->events            = neu_event_new();
    manager->plugin_manager    = neu_plugin_manager_create();
    manager->node_manager      = neu_node_manager_create();
    manager->subscribe_manager = neu_subscribe_manager_create();
    manager->template_manager  = neu_template_manager_create();

    rv = nng_pair1_open_poly(&manager->socket);
    assert(rv == 0);

    rv = nng_listen(manager->socket, url, NULL, 0);
    assert(rv == 0);

    nng_socket_set_int(manager->socket, NNG_OPT_RECVBUF, 8192);
    nng_socket_set_int(manager->socket, NNG_OPT_SENDBUF, 8292);
    nng_socket_set_ms(manager->socket, NNG_OPT_SENDTIMEO, 1000);
    nng_socket_get_int(manager->socket, NNG_OPT_RECVFD, &param.fd);
    manager->loop = neu_event_add_io(manager->events, param);

    manager->timestamp_lev_manager = 0;

    neu_metrics_init();
    start_static_adapter(manager, DEFAULT_DASHBOARD_PLUGIN_NAME);

    if (manager_load_plugin(manager) != 0) {
        nlog_warn("load plugin error");
    }

    UT_array *single_plugins =
        neu_plugin_manager_get_single(manager->plugin_manager);

    utarray_foreach(single_plugins, neu_resp_plugin_info_t *, plugin)
    {
        start_single_adapter(manager, plugin->single_name, plugin->name,
                             plugin->display);
    }
    utarray_free(single_plugins);

    if (manager_load_template(manager) != 0) {
        nlog_warn("load template error");
    }

    manager_load_node(manager);
    while (neu_node_manager_exist_uninit(manager->node_manager)) {
        usleep(1000 * 100);
    }

    manager_load_subscribe(manager);

    timer_level.usr_data = (void *) manager;
    manager->timer_lev   = neu_event_add_timer(manager->events, timer_level);

    timestamp_timer_param.usr_data = (void *) manager;
    manager->timer_timestamp =
        neu_event_add_timer(manager->events, timestamp_timer_param);

    nlog_notice("manager start");
    return manager;
}

void neu_manager_destroy(neu_manager_t *manager)
{
    neu_reqresp_head_t  header     = { .type = NEU_REQ_NODE_UNINIT };
    neu_req_node_init_t uninit     = { 0 };
    nng_msg *           uninit_msg = NULL;
    UT_array *pipes = neu_node_manager_get_pipes_all(manager->node_manager);

    neu_event_del_timer(manager->events, manager->timer_lev);
    neu_event_del_timer(manager->events, manager->timer_timestamp);
    strcpy(header.sender, "manager");
    utarray_foreach(pipes, nng_pipe *, pipe)
    {
        uninit_msg = neu_msg_gen(&header, &uninit);
        nng_msg_set_pipe(uninit_msg, *pipe);

        if (nng_sendmsg(manager->socket, uninit_msg, 0) != 0) {
            nng_msg_free(uninit_msg);
            nlog_warn("manager -> %d uninit msg send fail", (*pipe).id);
        }
    }
    utarray_free(pipes);

    while (1) {
        usleep(1000 * 100);
        if (neu_node_manager_size(manager->node_manager) == 0) {
            break;
        }
    }

    neu_subscribe_manager_destroy(manager->subscribe_manager);
    neu_node_manager_destroy(manager->node_manager);
    neu_plugin_manager_destroy(manager->plugin_manager);
    neu_template_manager_destroy(manager->template_manager);

    nng_close(manager->socket);
    neu_event_del_io(manager->events, manager->loop);
    neu_event_close(manager->events);

    free(manager);
    nlog_notice("manager exit");
}

const char *neu_manager_get_url()
{
    return url;
}

static int manager_level_check(void *usr_data)
{
    neu_manager_t *manager = (neu_manager_t *) usr_data;

    if (0 != manager->timestamp_lev_manager) {
        struct timeval   tv      = { 0 };
        int64_t          diff    = { 0 };
        int64_t          delay_s = 600;
        zlog_category_t *neuron  = zlog_get_category("neuron");

        gettimeofday(&tv, NULL);
        diff = tv.tv_sec - manager->timestamp_lev_manager;
        if (delay_s <= diff) {
            int ret = zlog_level_switch(neuron, default_log_level);
            if (0 != ret) {
                nlog_error("Modify default log level fail, ret:%d", ret);
            }
        }
    }

    return 0;
}

static int manager_loop(enum neu_event_io_type type, int fd, void *usr_data)
{
    neu_manager_t *     manager = (neu_manager_t *) usr_data;
    int                 rv      = 0;
    nng_msg *           msg     = NULL;
    neu_reqresp_head_t *header  = NULL;

    if (type == NEU_EVENT_IO_CLOSED || type == NEU_EVENT_IO_HUP) {
        nlog_warn("manager socket(%d) recv closed or hup %d.", fd, type);
        return 0;
    }

    rv = nng_recvmsg(manager->socket, &msg, NNG_FLAG_NONBLOCK);
    if (rv != 0) {
        nlog_warn("manager recv msg error: %d", rv);
        return 0;
    }

    header = (neu_reqresp_head_t *) nng_msg_body(msg);

    nlog_info("manager recv msg from: %s to %s, type: %s", header->sender,
              header->receiver, neu_reqresp_type_string(header->type));
    switch (header->type) {
    case NEU_REQRESP_TRANS_DATA: {
        neu_reqresp_trans_data_t *cmd = (neu_reqresp_trans_data_t *) &header[1];
        UT_array *apps = neu_subscribe_manager_find(manager->subscribe_manager,
                                                    cmd->driver, cmd->group);
        if (apps != NULL) {
            utarray_foreach(apps, neu_app_subscribe_t *, app)
            {
                forward_msg_dup(manager, msg, app->pipe);
                nlog_debug("forward trans data to pipe: %d", app->pipe.id);
            }
            utarray_free(apps);
            break;
        }
        break;
    }
    case NEU_REQ_UPDATE_LICENSE: {
        UT_array *pipes = neu_node_manager_get_pipes_all(manager->node_manager);

        utarray_foreach(pipes, nng_pipe *, pipe)
        {
            forward_msg_dup(manager, msg, *pipe);
            nlog_notice("forward license update to pipe: %d", pipe->id);
        }
        utarray_free(pipes);

        break;
    }
    case NEU_REQ_NODE_INIT: {
        neu_req_node_init_t *init = (neu_req_node_init_t *) &header[1];
        nng_pipe             pipe = nng_msg_get_pipe(msg);

        if (0 !=
            neu_node_manager_update(manager->node_manager, init->node, pipe)) {
            nlog_warn("bind node %s to pipe(%d) fail", init->node, pipe.id);
            break;
        }

        if (init->auto_start) {
            neu_adapter_t *adapter =
                neu_node_manager_find(manager->node_manager, init->node);
            neu_adapter_start(adapter);
        }

        nlog_notice("bind node %s to pipe(%d)", init->node, pipe.id);
        break;
    }
    case NEU_REQ_ADD_PLUGIN: {
        neu_req_add_plugin_t *cmd = (neu_req_add_plugin_t *) &header[1];
        int              error = neu_manager_add_plugin(manager, cmd->library);
        neu_resp_error_t e     = { .error = error };

        if (error == NEU_ERR_SUCCESS) {
            manager_strorage_plugin(manager);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_DEL_PLUGIN: {
        neu_req_del_plugin_t *cmd = (neu_req_del_plugin_t *) &header[1];
        int              error = neu_manager_del_plugin(manager, cmd->plugin);
        neu_resp_error_t e     = { .error = error };

        if (error == NEU_ERR_SUCCESS) {
            manager_strorage_plugin(manager);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_GET_PLUGIN: {
        UT_array *            plugins = neu_manager_get_plugins(manager);
        neu_resp_get_plugin_t resp    = { .plugins = plugins };

        header->type = NEU_RESP_GET_PLUGIN;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_ADD_TEMPLATE: {
        neu_resp_error_t        e   = { 0 };
        neu_req_add_template_t *cmd = (neu_req_add_template_t *) &header[1];

        e.error = neu_manager_add_template(manager, cmd->name, cmd->plugin,
                                           cmd->n_group, cmd->groups);
        if (NEU_ERR_SUCCESS == e.error) {
            manager_storage_add_template(manager, cmd->name);
        }

        neu_reqresp_template_fini(cmd);
        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_DEL_TEMPLATE: {
        neu_resp_error_t        e   = { 0 };
        neu_req_del_template_t *cmd = (neu_req_del_template_t *) &header[1];

        if (strlen(cmd->name) > 0) {
            e.error = neu_manager_del_template(manager, cmd->name);
            if (NEU_ERR_SUCCESS == e.error) {
                manager_storage_del_template(manager, cmd->name);
            }
        } else {
            neu_manager_clear_template(manager);
            manager_storage_clear_templates(manager);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_GET_TEMPLATE: {
        neu_resp_error_t        e    = { 0 };
        neu_resp_get_template_t resp = { 0 };
        neu_req_get_template_t *cmd  = (neu_req_get_template_t *) &header[1];

        strcpy(header->receiver, header->sender);

        e.error = neu_manager_get_template(manager, cmd->name, &resp);
        if (NEU_ERR_SUCCESS != e.error) {
            header->type = NEU_RESP_ERROR;
            reply(manager, header, &e);
            break;
        }

        header->type = NEU_RESP_GET_TEMPLATE;
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_GET_TEMPLATES: {
        neu_resp_error_t         e    = { 0 };
        neu_resp_get_templates_t resp = { 0 };

        strcpy(header->receiver, header->sender);

        e.error = neu_manager_get_templates(manager, &resp);
        if (NEU_ERR_SUCCESS != e.error) {
            header->type = NEU_RESP_ERROR;
            reply(manager, header, &e);
            break;
        }

        header->type = NEU_RESP_GET_TEMPLATES;
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_ADD_TEMPLATE_GROUP: {
        neu_req_add_template_group_t *cmd =
            (neu_req_add_template_group_t *) &header[1];
        neu_resp_error_t e = { 0 };

        if (cmd->interval < NEU_GROUP_INTERVAL_LIMIT) {
            e.error = NEU_ERR_GROUP_PARAMETER_INVALID;
        } else {
            e.error = neu_manager_add_template_group(manager, cmd->tmpl,
                                                     cmd->group, cmd->interval);
        }

        if (e.error == NEU_ERR_SUCCESS) {
            manager_storage_add_template_group(cmd->tmpl, cmd->group,
                                               cmd->interval);
        }

        neu_msg_exchange(header);
        header->type = NEU_RESP_ERROR;
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_DEL_TEMPLATE_GROUP: {
        neu_req_del_template_group_t *cmd =
            (neu_req_del_template_group_t *) &header[1];
        neu_resp_error_t e = { 0 };

        e.error = neu_manager_del_template_group(manager, cmd);

        if (e.error == NEU_ERR_SUCCESS) {
            manager_storage_del_template_group(cmd->tmpl, cmd->group);
        }

        neu_msg_exchange(header);
        header->type = NEU_RESP_ERROR;
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_UPDATE_TEMPLATE_GROUP: {
        neu_req_update_template_group_t *cmd =
            (neu_req_update_template_group_t *) &header[1];
        neu_resp_error_t e = { 0 };

        if (cmd->interval < NEU_GROUP_INTERVAL_LIMIT) {
            e.error = NEU_ERR_GROUP_PARAMETER_INVALID;
        } else {
            e.error = neu_manager_update_template_group(manager, cmd);
        }

        if (e.error == NEU_ERR_SUCCESS) {
            manager_storage_update_template_group(cmd->tmpl, cmd->group,
                                                  cmd->interval);
        }

        neu_msg_exchange(header);
        header->type = NEU_RESP_ERROR;
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_GET_TEMPLATE_GROUP: {
        neu_req_get_template_group_t *cmd =
            (neu_req_get_template_group_t *) &header[1];
        neu_resp_error_t     e    = { 0 };
        neu_resp_get_group_t resp = { 0 };

        neu_msg_exchange(header);

        e.error = neu_manager_get_template_group(manager, cmd, &resp.groups);
        if (NEU_ERR_SUCCESS != e.error) {
            header->type = NEU_RESP_ERROR;
            reply(manager, header, &e);
            break;
        }

        header->type = NEU_RESP_GET_GROUP;
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_ADD_TEMPLATE_TAG: {
        neu_req_add_template_tag_t *cmd =
            (neu_req_add_template_tag_t *) &header[1];
        neu_resp_add_tag_t resp = { 0 };

        resp.error = neu_manager_add_template_tags(
            manager, cmd->tmpl, cmd->group, cmd->n_tag, cmd->tags, &resp.index);
        if (resp.index > 0) {
            manager_storage_add_template_tags(cmd->tmpl, cmd->group, cmd->tags,
                                              cmd->n_tag);
        }

        neu_req_add_template_tag_fini(cmd);

        header->type = NEU_RESP_ADD_TEMPLATE_TAG;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_UPDATE_TEMPLATE_TAG: {
        neu_req_update_template_tag_t *cmd =
            (neu_req_update_template_tag_t *) &header[1];
        neu_resp_update_tag_t resp = { 0 };

        resp.error =
            neu_manager_update_template_tags(manager, cmd, &resp.index);
        if (resp.index > 0) {
            manager_storage_update_template_tags(cmd->tmpl, cmd->group,
                                                 cmd->tags, cmd->n_tag);
        }

        neu_req_update_template_tag_fini(cmd);

        header->type = NEU_RESP_UPDATE_TEMPLATE_TAG;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_DEL_TEMPLATE_TAG: {
        neu_req_del_template_tag_t *cmd =
            (neu_req_del_template_tag_t *) &header[1];
        neu_resp_error_t resp = { 0 };

        resp.error = neu_manager_del_template_tags(manager, cmd);
        if (0 == resp.error) {
            manager_storage_del_template_tags(cmd->tmpl, cmd->group,
                                              (const char *const *) cmd->tags,
                                              cmd->n_tag);
        }

        neu_req_del_template_tag_fini(cmd);

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_GET_TEMPLATE_TAG: {
        neu_req_get_template_tag_t *cmd =
            (neu_req_get_template_tag_t *) &header[1];
        neu_resp_error_t   e    = { 0 };
        neu_resp_get_tag_t resp = { 0 };

        e.error = neu_manager_get_template_tags(manager, cmd, &resp.tags);

        strcpy(header->receiver, header->sender);
        if (0 == e.error) {
            header->type = NEU_RESP_GET_TEMPLATE_TAG;
            reply(manager, header, &resp);
        } else {
            header->type = NEU_RESP_ERROR;
            reply(manager, header, &e);
        }
        break;
    }
    case NEU_REQ_INST_TEMPLATE: {
        neu_req_inst_template_t *cmd = (neu_req_inst_template_t *) &header[1];
        neu_resp_error_t         e   = { 0 };

        e.error = neu_manager_instantiate_template(manager, cmd);
        if (NEU_ERR_SUCCESS == e.error) {
            manager_storage_inst_node(manager, cmd->tmpl, cmd->node);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_ADD_NODE: {
        neu_req_add_node_t *cmd = (neu_req_add_node_t *) &header[1];
        int                 error =
            neu_manager_add_node(manager, cmd->node, cmd->plugin, false);
        neu_resp_error_t e = { .error = error };

        if (error == NEU_ERR_SUCCESS) {
            manager_storage_add_node(manager, cmd->node);
            notify_monitor(manager, NEU_REQ_ADD_NODE_EVENT, cmd);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &e);
        break;
    }
    case NEU_REQ_UPDATE_NODE: {
        neu_req_update_node_t *cmd = (neu_req_update_node_t *) &header[1];
        neu_resp_error_t       e   = { 0 };

        if (0 == strcmp("monitor", cmd->node)) {
            e.error = NEU_ERR_NODE_NOT_ALLOW_UPDATE;
        } else if (0 == strcmp("monitor", cmd->new_name)) {
            e.error = NEU_ERR_NODE_EXIST;
        } else if (NULL ==
                   neu_node_manager_find(manager->node_manager, cmd->node)) {
            e.error = NEU_ERR_NODE_NOT_EXIST;
        } else if (NULL !=
                   neu_node_manager_find(manager->node_manager,
                                         cmd->new_name)) {
            // this also makes renaming to the original name an error
            e.error = NEU_ERR_NODE_EXIST;
        }

        if (0 == e.error) {
            header->type = NEU_REQ_NODE_RENAME;
            forward_msg(manager, msg, header->receiver);
        } else {
            header->type = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
        }

        break;
    }
    case NEU_REQ_DEL_NODE: {
        neu_req_del_node_t *cmd   = (neu_req_del_node_t *) &header[1];
        neu_resp_error_t    error = { 0 };
        neu_adapter_t *     adapter =
            neu_node_manager_find(manager->node_manager, cmd->node);
        bool single =
            neu_node_manager_is_single(manager->node_manager, cmd->node);

        strcpy(header->receiver, cmd->node);
        if (adapter == NULL) {
            error.error  = NEU_ERR_NODE_NOT_EXIST;
            header->type = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &error);
            break;
        }

        if (single) {
            error.error  = NEU_ERR_NODE_NOT_ALLOW_DELETE;
            header->type = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &error);
            break;
        }

        manager_storage_del_node(manager, cmd->node);
        if (neu_adapter_get_type(adapter) == NEU_NA_TYPE_APP) {
            neu_subscribe_manager_unsub_all(manager->subscribe_manager,
                                            cmd->node);
        }
        header->type = NEU_REQ_NODE_UNINIT;
        forward_msg(manager, msg, header->receiver);
        notify_monitor(manager, NEU_REQ_DEL_NODE_EVENT, cmd);
        if (neu_adapter_get_type(adapter) == NEU_NA_TYPE_DRIVER) {
            UT_array *apps = neu_subscribe_manager_find_by_driver(
                manager->subscribe_manager, cmd->node);

            utarray_foreach(apps, neu_app_subscribe_t *, app)
            {
                neu_reqresp_node_deleted_t resp = { 0 };
                header->type                    = NEU_REQRESP_NODE_DELETED;

                strcpy(resp.node, header->receiver);
                strcpy(header->receiver, app->app_name);
                strcpy(header->sender, "manager");
                reply(manager, header, &resp);
            }
            utarray_free(apps);
        }

        break;
    }
    case NEU_RESP_NODE_UNINIT: {
        neu_resp_node_uninit_t *cmd = (neu_resp_node_uninit_t *) &header[1];

        neu_manager_del_node(manager, cmd->node);
        if (strlen(header->receiver) > 0 &&
            strcmp(header->receiver, "manager") != 0) {
            neu_resp_error_t error = { 0 };
            header->type           = NEU_RESP_ERROR;
            reply(manager, header, &error);
        }
        break;
    }
    case NEU_REQ_GET_NODE: {
        neu_req_get_node_t *cmd = (neu_req_get_node_t *) &header[1];
        UT_array *          nodes =
            neu_manager_get_nodes(manager, cmd->type, cmd->plugin, cmd->node);
        neu_resp_get_node_t resp = { .nodes = nodes };

        header->type = NEU_RESP_GET_NODE;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_SUBSCRIBE_GROUP: {
        neu_req_subscribe_t *cmd   = (neu_req_subscribe_t *) &header[1];
        neu_resp_error_t     error = { 0 };

        error.error = neu_manager_subscribe(manager, cmd->app, cmd->driver,
                                            cmd->group, cmd->params);

        if (error.error == NEU_ERR_SUCCESS) {
            forward_msg(manager, msg, cmd->app);
            manager_storage_subscribe(manager, cmd->app, cmd->driver,
                                      cmd->group, cmd->params);
        } else {
            free(cmd->params);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &error);
        break;
    }
    case NEU_REQ_UNSUBSCRIBE_GROUP: {
        neu_req_unsubscribe_t *cmd   = (neu_req_unsubscribe_t *) &header[1];
        neu_resp_error_t       error = { 0 };

        error.error =
            neu_manager_unsubscribe(manager, cmd->app, cmd->driver, cmd->group);

        if (error.error == NEU_ERR_SUCCESS) {
            forward_msg(manager, msg, cmd->app);
            manager_storage_unsubscribe(manager, cmd->app, cmd->driver,
                                        cmd->group);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &error);
        break;
    }
    case NEU_REQ_GET_SUBSCRIBE_GROUP: {
        neu_req_get_subscribe_group_t *cmd =
            (neu_req_get_subscribe_group_t *) &header[1];
        UT_array *groups =
            neu_manager_get_sub_group_deep_copy(manager, cmd->app);
        neu_resp_get_subscribe_group_t resp = { .groups = groups };

        strcpy(header->receiver, header->sender);
        header->type = NEU_RESP_GET_SUBSCRIBE_GROUP;
        reply(manager, header, &resp);
        break;
    }
    case NEU_REQ_GET_SUB_DRIVER_TAGS: {
        neu_req_get_sub_driver_tags_t *cmd =
            (neu_req_get_sub_driver_tags_t *) &header[1];
        neu_resp_get_sub_driver_tags_t resp = { 0 };
        UT_array *groups = neu_manager_get_sub_group(manager, cmd->app);

        utarray_new(resp.infos, neu_resp_get_sub_driver_tags_info_icd());
        utarray_foreach(groups, neu_resp_subscribe_info_t *, info)
        {
            neu_resp_get_sub_driver_tags_info_t in = { 0 };
            neu_adapter_t *                     driver =
                neu_node_manager_find(manager->node_manager, info->driver);
            assert(driver != NULL);

            strcpy(in.driver, info->driver);
            strcpy(in.group, info->group);
            neu_adapter_driver_get_value_tag((neu_adapter_driver_t *) driver,
                                             info->group, &in.tags);

            utarray_push_back(resp.infos, &in);
        }
        utarray_free(groups);

        strcpy(header->receiver, header->sender);
        header->type = NEU_RESP_GET_SUB_DRIVER_TAGS;
        reply(manager, header, &resp);

        break;
    }
    case NEU_REQ_GET_NODES_STATE: {
        neu_resp_get_nodes_state_t resp = { 0 };
        UT_array *states = neu_node_manager_get_state(manager->node_manager);

        resp.states = utarray_clone(states);

        strcpy(header->receiver, header->sender);
        strcpy(header->sender, "manager");
        header->type = NEU_RESP_GET_NODES_STATE;
        reply(manager, header, &resp);

        utarray_free(states);
        break;
    }
    case NEU_REQ_GET_DRIVER_GROUP: {
        neu_resp_get_driver_group_t resp = { 0 };

        resp.groups = neu_manager_get_driver_group(manager);

        strcpy(header->receiver, header->sender);
        strcpy(header->sender, "manager");
        header->type = NEU_RESP_GET_DRIVER_GROUP;
        reply(manager, header, &resp);

        break;
    }
    case NEU_REQ_GET_GROUP:
    case NEU_REQ_GET_NODE_SETTING:
    case NEU_REQ_READ_GROUP:
    case NEU_REQ_WRITE_TAG:
    case NEU_REQ_WRITE_TAGS:
    case NEU_REQ_GET_NODE_STATE:
    case NEU_REQ_GET_TAG:
    case NEU_REQ_GET_NDRIVER_TAGS:
    case NEU_REQ_NODE_CTL:
    case NEU_REQ_UPDATE_GROUP:
    case NEU_REQ_ADD_GROUP: {
        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
        } else {
            forward_msg(manager, msg, header->receiver);
        }

        break;
    }

    case NEU_RESP_NODE_RENAME: {
        neu_resp_node_rename_t *resp = (neu_resp_node_rename_t *) &header[1];
        if (0 == resp->error) {
            neu_manager_update_node_name(manager, resp->node, resp->new_name);
            manager_storage_update_node(manager, resp->node, resp->new_name);
        }

        neu_resp_error_t e = { .error = resp->error };
        header->type       = NEU_RESP_ERROR;
        reply(manager, header, &e);
        break;
    }

    case NEU_REQ_ADD_NODE_EVENT:
    case NEU_REQ_DEL_NODE_EVENT:
    case NEU_REQ_NODE_CTL_EVENT:
    case NEU_REQ_ADD_GROUP_EVENT:
    case NEU_REQ_DEL_GROUP_EVENT:
    case NEU_REQ_UPDATE_GROUP_EVENT:
    case NEU_REQ_ADD_TAG_EVENT:
    case NEU_REQ_DEL_TAG_EVENT:
    case NEU_REQ_UPDATE_TAG_EVENT: {
        if (neu_node_manager_find(manager->node_manager, header->receiver)) {
            forward_msg(manager, msg, header->receiver);
        }

        break;
    }

    case NEU_REQ_DEL_GROUP: {
        neu_req_del_group_t *cmd = (neu_req_del_group_t *) &header[1];

        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
        } else {
            forward_msg(manager, msg, header->receiver);
            neu_subscribe_manager_remove(manager->subscribe_manager,
                                         cmd->driver, cmd->group);
        }
        break;
    }
    case NEU_REQ_DEL_TAG: {
        neu_req_del_tag_t *cmd = (neu_req_del_tag_t *) &header[1];

        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
            for (int i = 0; i < cmd->n_tag; i++) {
                free(cmd->tags[i]);
            }
            free(cmd->tags);
        } else {
            forward_msg(manager, msg, header->receiver);
        }

        break;
    }
    case NEU_REQ_UPDATE_TAG:
    case NEU_REQ_ADD_TAG: {
        neu_req_add_tag_t *cmd = (neu_req_add_tag_t *) &header[1];

        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
            for (int i = 0; i < cmd->n_tag; i++) {
                neu_tag_fini(&cmd->tags[i]);
            }
            free(cmd->tags);
        } else {
            forward_msg(manager, msg, header->receiver);
        }

        break;
    }
    case NEU_REQ_NODE_SETTING:
    case NEU_REQ_NODE_SETTING_EVENT: {
        neu_req_node_setting_t *cmd = (neu_req_node_setting_t *) &header[1];

        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
            free(cmd->setting);
        } else {
            forward_msg(manager, msg, header->receiver);
        }
        break;
    }

    case NEU_RESP_ADD_TAG:
    case NEU_RESP_ADD_TEMPLATE_TAG:
    case NEU_RESP_UPDATE_TAG:
    case NEU_RESP_UPDATE_TEMPLATE_TAG:
    case NEU_RESP_GET_TAG:
    case NEU_RESP_GET_TEMPLATE_TAG:
    case NEU_RESP_GET_NDRIVER_TAGS:
    case NEU_RESP_GET_GROUP:
    case NEU_RESP_GET_NODE_SETTING:
    case NEU_RESP_GET_NODE_STATE:
    case NEU_RESP_ERROR:
    case NEU_RESP_READ_GROUP:
        forward_msg(manager, msg, header->receiver);
        break;

    case NEU_REQ_ADD_NDRIVER_MAP: {
        neu_req_ndriver_map_t *cmd   = (neu_req_ndriver_map_t *) &header[1];
        neu_resp_error_t       error = { 0 };

        error.error = neu_manager_add_ndriver_map(manager, cmd->ndriver,
                                                  cmd->driver, cmd->group);
        if (error.error == NEU_ERR_SUCCESS) {
            forward_msg(manager, msg, cmd->ndriver);
            manager_storage_add_ndriver_map(manager, cmd->ndriver, cmd->driver,
                                            cmd->group);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &error);
        break;
    }
    case NEU_REQ_DEL_NDRIVER_MAP: {
        neu_req_ndriver_map_t *cmd   = (neu_req_ndriver_map_t *) &header[1];
        neu_resp_error_t       error = { 0 };

        error.error = neu_manager_del_ndriver_map(manager, cmd->ndriver,
                                                  cmd->driver, cmd->group);
        if (error.error == NEU_ERR_SUCCESS) {
            forward_msg(manager, msg, cmd->ndriver);
            manager_storage_del_ndriver_map(manager, cmd->ndriver, cmd->driver,
                                            cmd->group);
        }

        header->type = NEU_RESP_ERROR;
        strcpy(header->receiver, header->sender);
        reply(manager, header, &error);
        break;
    }
    case NEU_REQ_GET_NDRIVER_MAPS: {
        neu_req_get_ndriver_maps_t *cmd =
            (neu_req_get_ndriver_maps_t *) &header[1];
        neu_resp_error_t            e    = { 0 };
        neu_resp_get_ndriver_maps_t resp = { 0 };

        e.error =
            neu_manager_get_ndriver_maps(manager, cmd->ndriver, &resp.groups);

        strcpy(header->receiver, header->sender);
        if (0 == e.error) {
            header->type = NEU_RESP_GET_NDRIVER_MAPS;
            reply(manager, header, &resp);
        } else {
            header->type = NEU_RESP_ERROR;
            reply(manager, header, &e);
        }

        break;
    }
    case NEU_REQ_UPDATE_NDRIVER_TAG_PARAM:
    case NEU_REQ_UPDATE_NDRIVER_TAG_INFO: {
        if (NULL !=
            neu_node_manager_find(manager->node_manager, header->receiver)) {
            forward_msg(manager, msg, header->receiver);
            break;
        }

        neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
        header->type       = NEU_RESP_ERROR;
        neu_msg_exchange(header);
        reply(manager, header, &e);

        if (NEU_REQ_UPDATE_NDRIVER_TAG_PARAM == header->type) {
            neu_req_update_ndriver_tag_param_fini(
                (neu_req_update_ndriver_tag_param_t *) &header[1]);
        } else {
            neu_req_update_ndriver_tag_info_fini(
                (neu_req_update_ndriver_tag_info_t *) &header[1]);
        }
        break;
    }
    case NEU_REQ_UPDATE_LOG_LEVEL:
        if (neu_node_manager_find(manager->node_manager, header->receiver) ==
            NULL) {
            neu_resp_error_t e = { .error = NEU_ERR_NODE_NOT_EXIST };
            header->type       = NEU_RESP_ERROR;
            neu_msg_exchange(header);
            reply(manager, header, &e);
        } else {
            struct timeval tv = { 0 };
            gettimeofday(&tv, NULL);
            manager->timestamp_lev_manager = tv.tv_sec;

            forward_msg(manager, msg, header->receiver);
        }

        break;
    default:
        assert(false);
        break;
    }

    nng_msg_free(msg);
    return 0;
}

inline static void forward_msg_dup(neu_manager_t *manager, nng_msg *msg,
                                   nng_pipe pipe)
{
    nng_msg *out_msg;

    nng_msg_dup(&out_msg, msg);
    nng_msg_set_pipe(out_msg, pipe);
    if (nng_sendmsg(manager->socket, out_msg, 0) == 0) {
        nlog_info("forward msg to pipe %d", pipe.id);
    } else {
        nlog_warn("forward msg to pipe %d fail", pipe.id);
        nng_msg_free(out_msg);
    }
}

inline static void forward_msg(neu_manager_t *manager, nng_msg *msg,
                               const char *node)
{
    nng_msg *out_msg;
    nng_pipe pipe = neu_node_manager_get_pipe(manager->node_manager, node);

    nng_msg_dup(&out_msg, msg);
    nng_msg_set_pipe(out_msg, pipe);
    if (nng_sendmsg(manager->socket, out_msg, 0) == 0) {
        nlog_info("forward msg to %s", node);
    } else {
        nlog_warn("forward msg to pipe (%s)%d fail", node, pipe.id);
        nng_msg_free(out_msg);
    }
}

inline static void notify_monitor(neu_manager_t *    manager,
                                  neu_reqresp_type_e event, void *data)
{
    nng_pipe pipe = neu_node_manager_get_pipe(manager->node_manager, "monitor");
    if (0 == pipe.id) {
        nlog_error("no monitor node");
        return;
    }

    neu_reqresp_head_t header = {
        .sender   = "manager",
        .receiver = "monitor",
        .type     = event,
    };

    nng_msg *msg = neu_msg_gen(&header, data);
    nng_msg_set_pipe(msg, pipe);

    int ret = nng_sendmsg(manager->socket, msg, 0);
    if (ret != 0) {
        nng_msg_free(msg);
        nlog_warn("notify %s of %s, error: %s", header.receiver,
                  neu_reqresp_type_string(header.type), nng_strerror(ret));
    }
}

static void start_static_adapter(neu_manager_t *manager, const char *name)
{
    neu_adapter_t *       adapter      = NULL;
    neu_plugin_instance_t instance     = { 0 };
    neu_adapter_info_t    adapter_info = {
        .name = name,
    };

    neu_plugin_manager_load_static(manager->plugin_manager, name, &instance);
    adapter_info.handle = instance.handle;
    adapter_info.module = instance.module;

    adapter = neu_adapter_create(&adapter_info);
    neu_node_manager_add_static(manager->node_manager, adapter);
    neu_adapter_init(adapter, false);
    neu_adapter_start(adapter);
}

static void start_single_adapter(neu_manager_t *manager, const char *name,
                                 const char *plugin_name, bool display)
{
    neu_adapter_t *       adapter      = NULL;
    neu_plugin_instance_t instance     = { 0 };
    neu_adapter_info_t    adapter_info = {
        .name = name,
    };

    if (0 !=
        neu_plugin_manager_create_instance(manager->plugin_manager, plugin_name,
                                           &instance)) {
        return;
    }

    adapter_info.handle = instance.handle;
    adapter_info.module = instance.module;
    adapter             = neu_adapter_create(&adapter_info);

    neu_node_manager_add_single(manager->node_manager, adapter, display);
    if (display) {
        manager_storage_add_node(manager, name);
    }

    neu_adapter_init(adapter, false);
    neu_adapter_start_single(adapter);
}

inline static void reply(neu_manager_t *manager, neu_reqresp_head_t *header,
                         void *data)
{
    nng_msg *msg = neu_msg_gen(header, data);
    nng_pipe pipe =
        neu_node_manager_get_pipe(manager->node_manager, header->receiver);

    nng_msg_set_pipe(msg, pipe);
    int ret = nng_sendmsg(manager->socket, msg, 0);

    if (ret != 0) {
        nng_msg_free(msg);
        nlog_warn("reply %s to %s, error: %d",
                  neu_reqresp_type_string(header->type), header->receiver, ret);
    }
}

static int update_timestamp(void *usr_data)
{
    (void) usr_data;
    global_timestamp = neu_time_ms();
    return 0;
}