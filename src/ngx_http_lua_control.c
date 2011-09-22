#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_http_lua_control.h"


static int ngx_http_lua_ngx_exec(lua_State *L);
static int ngx_http_lua_ngx_redirect(lua_State *L);
static int ngx_http_lua_ngx_exit(lua_State *L);


void
ngx_http_lua_inject_control_api(lua_State *L)
{
    lua_pushcfunction(L, ngx_http_lua_ngx_redirect);
    lua_setfield(L, -2, "redirect");

    lua_pushcfunction(L, ngx_http_lua_ngx_exec);
    lua_setfield(L, -2, "exec");

    lua_pushcfunction(L, ngx_http_lua_ngx_exit);
    lua_setfield(L, -2, "throw_error"); /* deprecated */

    lua_pushcfunction(L, ngx_http_lua_ngx_exit);
    lua_setfield(L, -2, "exit");
}


static int
ngx_http_lua_ngx_exec(lua_State *L)
{
    int                          n;
    ngx_http_request_t          *r;
    ngx_http_lua_ctx_t          *ctx;
    ngx_str_t                    uri;
    ngx_str_t                    args, user_args;
    ngx_uint_t                   flags;
    u_char                      *p;
    u_char                      *q;
    size_t                       len;

    n = lua_gettop(L);
    if (n != 1 && n != 2) {
        return luaL_error(L, "expecting one or two arguments, but got %d",
                n);
    }

    lua_getglobal(L, GLOBALS_SYMBOL_REQUEST);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    args.data = NULL;
    args.len = 0;

    /* read the 1st argument (uri) */

    p = (u_char *) luaL_checklstring(L, 1, &len);

    if (len == 0) {
        return luaL_error(L, "The uri argument is empty");
    }

    uri.data = ngx_palloc(r->pool, len);
    ngx_memcpy(uri.data, p, len);

    uri.len = len;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    if (ngx_http_parse_unsafe_uri(r, &uri, &args, &flags)
            != NGX_OK) {
        ctx->headers_sent = 1;
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (n == 2) {
        /* read the 2nd argument (args) */
        p = (u_char *) luaL_checklstring(L, 2, &len);

        user_args.data = ngx_palloc(r->pool, len);
        ngx_memcpy(user_args.data, p, len);

        user_args.len = len;

    } else {
        user_args.data = NULL;
        user_args.len = 0;
    }

    if (user_args.len) {
        if (args.len == 0) {
            args = user_args;
        } else {
            p = ngx_palloc(r->pool, args.len + user_args.len + 1);
            if (p == NULL) {
                return luaL_error(L, "out of memory");
            }

            q = ngx_copy(p, args.data, args.len);
            *q++ = '&';
            q = ngx_copy(q, user_args.data, user_args.len);

            args.data = p;
            args.len += user_args.len + 1;
        }
    }

    if (ctx->headers_sent) {
        return luaL_error(L, "attempt to call ngx.exec after "
                "sending out response headers");
    }

    ctx->exec_uri = uri;
    ctx->exec_args = args;

    return lua_yield(L, 0);
}


static int
ngx_http_lua_ngx_redirect(lua_State *L)
{
    ngx_http_lua_ctx_t          *ctx;
    ngx_int_t                    rc;
    int                          n;
    u_char                      *p;
    u_char                      *uri;
    size_t                       len;
    ngx_http_request_t          *r;

    n = lua_gettop(L);

    if (n != 1 && n != 2) {
        return luaL_error(L, "expecting one or two arguments");
    }

    p = (u_char *) luaL_checklstring(L, 1, &len);

    if (n == 2) {
        rc = (ngx_int_t) luaL_checknumber(L, 2);

        if (rc != NGX_HTTP_MOVED_TEMPORARILY &&
                rc != NGX_HTTP_MOVED_PERMANENTLY)
        {
            return luaL_error(L, "only ngx.HTTP_MOVED_TEMPORARILY and "
                    "ngx.HTTP_MOVED_PERMANENTLY are allowed");
        }
    } else {
        rc = NGX_HTTP_MOVED_TEMPORARILY;
    }

    lua_getglobal(L, GLOBALS_SYMBOL_REQUEST);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    if (ctx->headers_sent) {
        return luaL_error(L, "attempt to call ngx.redirect after sending out "
                "the headers");
    }

    uri = ngx_palloc(r->pool, len);
    if (uri == NULL) {
        return luaL_error(L, "out of memory");
    }

    ngx_memcpy(uri, p, len);

    r->headers_out.location = ngx_list_push(&r->headers_out.headers);
    if (r->headers_out.location == NULL) {
        return luaL_error(L, "out of memory");
    }

    r->headers_out.location->hash = 1;
    r->headers_out.location->value.len = len;
    r->headers_out.location->value.data = uri;
    ngx_str_set(&r->headers_out.location->key, "Location");

    r->headers_out.status = rc;

    ctx->exit_code = rc;
    ctx->exited = 1;

    return lua_yield(L, 0);
}


static int
ngx_http_lua_ngx_exit(lua_State *L)
{
    ngx_http_request_t          *r;
    ngx_http_lua_ctx_t          *ctx;
    ngx_int_t                    rc;

    if (lua_gettop(L) != 1) {
        return luaL_error(L, "expecting one argument");
    }

    lua_getglobal(L, GLOBALS_SYMBOL_REQUEST);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    rc = (ngx_int_t) luaL_checkinteger(L, 1);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE && ctx->headers_sent) {
        return luaL_error(L, "attempt to call ngx.exit after sending "
                "out the headers");
    }

    ctx->exit_code = rc;
    ctx->exited = 1;

    dd("calling yield");
    return lua_yield(L, 0);
}

