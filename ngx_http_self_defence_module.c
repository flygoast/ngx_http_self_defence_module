/*
 * Copyright (c) 2012, FengGu <flygoast@126.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_HAVE_SYSVSHM)


#include <sys/ipc.h>
#include <sys/shm.h>


typedef struct {
    ngx_int_t   value;
    ngx_str_t   action;
    ngx_int_t   ratio;
} ngx_http_self_defence_action_t;


typedef struct {
    ngx_int_t   shm_key;
    ngx_int_t   shm_len;
    int         shm_id;
    u_char     *shm_base;
} ngx_http_self_defence_main_conf_t;


typedef struct {
    ngx_int_t     defence_at; 
    ngx_array_t  *defence_actions;
} ngx_http_self_defence_loc_conf_t;


static ngx_int_t ngx_http_self_defence_do_redirect(ngx_http_request_t *r, 
    ngx_str_t *path);
static ngx_int_t ngx_http_self_defence_handler(ngx_http_request_t *r);
static char *ngx_http_defence_shm(ngx_conf_t *cf, ngx_command_t *cmd, 
    void *conf);
static char *ngx_http_defence_action(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_defence_at_post(ngx_conf_t *cf, void *post, void *data);
static void *ngx_http_self_defence_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_self_defence_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_self_defence_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_self_defence_init(ngx_conf_t *cf);

static ngx_conf_post_handler_pt ngx_http_self_defence_at_post_p =
    ngx_http_defence_at_post;

static ngx_command_t ngx_http_self_defence_commands[] = {

    { ngx_string("defence_shm"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE12,
      ngx_http_defence_shm,
      0,
      0,
      NULL },

    { ngx_string("defence_at"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_self_defence_loc_conf_t, defence_at),
      &ngx_http_self_defence_at_post_p },

    { ngx_string("defence_action"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE123,
      ngx_http_defence_action,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_self_defence_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_http_self_defence_init,             /* postconfiguration */

    ngx_http_self_defence_create_main_conf, /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_http_self_defence_create_loc_conf,  /* create location configuration */
    ngx_http_self_defence_merge_loc_conf    /* merge location configuration */
};

ngx_module_t  ngx_http_self_defence_module = {
    NGX_MODULE_V1,
    &ngx_http_self_defence_module_ctx,      /* module context */
    ngx_http_self_defence_commands,         /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_self_defence_do_redirect(ngx_http_request_t *r, ngx_str_t *path)
{
    if (path->len == 0) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;

    } else if (path->data[0] == '@') {
        (void)ngx_http_named_location(r, path);

    } else {
        (void)ngx_http_internal_redirect(r, path, &r->args);
    }

    ngx_http_finalize_request(r, NGX_DONE);

    return NGX_DONE;
}

static ngx_int_t
ngx_http_self_defence_handler(ngx_http_request_t *r)
{
    ngx_uint_t                           i;
    u_char                               value;
    ngx_http_self_defence_main_conf_t   *dmcf;
    ngx_http_self_defence_loc_conf_t    *dlcf;
    ngx_http_self_defence_action_t      *action;

    dmcf = ngx_http_get_module_main_conf(r, ngx_http_self_defence_module);

    if (dmcf->shm_key == NGX_CONF_UNSET) {
        return NGX_DECLINED;
    }

    if (r->main->count != 1) {
        return NGX_DECLINED;
    }

    dlcf = ngx_http_get_module_loc_conf(r, ngx_http_self_defence_module);

    if (dlcf->defence_actions == NGX_CONF_UNSET_PTR) {
        return NGX_DECLINED;
    }

    value = *(dmcf->shm_base + dlcf->defence_at);

    action = dlcf->defence_actions->elts;
    
    for (i = 0; i < dlcf->defence_actions->nelts; i++) {

        if (action[i].value == value) {

            if (action->ratio == 0) {
                continue;
            }

            if (action->ratio != 100) {
                if (r->connection->number % 100 >= action->ratio) {
                    continue;
                }
            }

            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                "self_defence limited, shm pos: %i, value: %i, action: \"%V\"",
                 dlcf->defence_at, value, &action[i].action);
            return ngx_http_self_defence_do_redirect(r, &action[i].action);
        }
    }

    return NGX_DECLINED;
}


static char *
ngx_http_defence_shm(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_self_defence_main_conf_t  *dmcf;
    ngx_str_t                          *value;

    dmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_self_defence_module);

    if (dmcf->shm_key != NGX_CONF_UNSET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "defence_shm directive is duplicate");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    dmcf->shm_key = ngx_atoi(value[1].data, value[1].len);
    if (dmcf->shm_key == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts == 3) {
        dmcf->shm_len = ngx_atoi(value[2].data, value[2].len);
        if (dmcf->shm_len == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid shm_length \"%V\"", &value[2]);
            return NGX_CONF_ERROR;

        } else if (dmcf->shm_len < 1 || dmcf->shm_len > 255) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid shm_length \"%V\", must between 1 and 255",
                           &value[2]);
            return NGX_CONF_ERROR;
        }

    } else {
        dmcf->shm_len = 8;
    }

    dmcf->shm_id = shmget((key_t)dmcf->shm_key, dmcf->shm_len, 0666|IPC_CREAT);
    if (dmcf->shm_id == -1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "shmget(%i, %i, 0666|IPC_CREAT) failed",
                           dmcf->shm_key, dmcf->shm_len);
        return NGX_CONF_ERROR;
    }

    dmcf->shm_base = (u_char *)shmat(dmcf->shm_id, NULL, 0);
    if (dmcf->shm_base == (u_char *)-1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "shmat(%i, NULL, 0) failed", dmcf->shm_id);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_defence_at_post(ngx_conf_t *cf, void *post, void *data)
{
    ngx_http_self_defence_main_conf_t  *dmcf;
    ngx_int_t                          *np = data;

    dmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_self_defence_module);

    if (*np < 0 || *np >= dmcf->shm_len) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "defence_at value must be between 0 and %i",
                           dmcf->shm_len);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_defence_action(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_self_defence_main_conf_t   *dmcf;
    ngx_http_self_defence_loc_conf_t    *dlcf;
    ngx_http_self_defence_action_t      *action;
    ngx_str_t                           *value;
    ngx_uint_t                           i, j;
    ngx_int_t                            n;

    dmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_self_defence_module);
    if (dmcf->shm_base == (u_char *)-1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "defense_shm must be specified first");
        return NGX_CONF_ERROR;
    }

    dlcf = (ngx_http_self_defence_loc_conf_t *)conf;

    value = cf->args->elts;
    i = 1;

    n = ngx_atoi(value[i].data, value[i].len);

    /* We just use a byte as a value. */
    if (n == NGX_ERROR || n > 255) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid value \"%i\", must between 0 and 255", n);
        return NGX_CONF_ERROR;
    }
    
    if (dlcf->defence_actions == NGX_CONF_UNSET_PTR) {
        dlcf->defence_actions = ngx_array_create(cf->pool, 4, 
                                        sizeof(ngx_http_self_defence_action_t));
        if (dlcf->defence_actions == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    action = ngx_array_push(dlcf->defence_actions);
    if (action == NULL) {
        return NGX_CONF_ERROR;
    }

    action->value = n;

    /*
     * set defaute values
     */
    action->action.len = 0;
    action->action.data = NULL;
    action->ration = 100;

    i++;

    if (cf->args->nelts >= 3) {
        if (value[i].data[0] != '@' && value[i].data[0] != '/') {

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid action value \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        action->action.data = value[i].data;
        action->action.len = value[i].len;

        i++;

        if (cf->args->nelts == 4) {
            action->ratio = ngx_atoi(value[i].data, value[i].len);
            if (action->ratio == NGX_ERROR || 
                (action->ratio < 0 || action->ratio > 100))
            {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                "invalid  value \"%i\", must between 0 and 255",
                                action->ratio);
                return NGX_CONF_ERROR;
            }
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_self_defence_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_self_defence_main_conf_t  *dmcf;

    dmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_self_defence_main_conf_t));
    if (dmcf == NULL) {
        return NULL;
    }

    dmcf->shm_key = NGX_CONF_UNSET;
    dmcf->shm_len = NGX_CONF_UNSET;
    dmcf->shm_id = -1;
    dmcf->shm_base = (u_char *)-1;

    return dmcf;
}


static void *
ngx_http_self_defence_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_self_defence_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_self_defence_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->defence_at = NGX_CONF_UNSET;
    conf->defence_actions = NGX_CONF_UNSET_PTR;

    return conf;
}


static char *
ngx_http_self_defence_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_self_defence_loc_conf_t *prev = parent;
    ngx_http_self_defence_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->defence_at, prev->defence_at, 0);
    ngx_conf_merge_ptr_value(conf->defence_actions, prev->defence_actions, 
                             NGX_CONF_UNSET_PTR);
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_self_defence_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt         *h;
    ngx_http_core_main_conf_t   *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_self_defence_handler;

    return NGX_OK;
}


#else

#error "self_defence module need SYSV shared memory!"

#endif /* (NGX_HAVE_SYSVSHM) */
