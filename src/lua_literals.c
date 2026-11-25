/* Extra Lua C function for loading GPT-compiled source. The stock loader,
   string.dump, and the public Lua API retain their original behavior. */
#define LUA_CORE
#include "lauxlib.h"
#include "lstate.h"
#include "ltable.h"
#include "lgc.h"

static TString *canonical(lua_State *L, Table *seen, Proto *owner, TString *s) {
    if (!s || s->tt != LUA_VLNGSTR)
        return s;
    const TValue *value = luaH_getstr(seen, s);
    if (ttisnil(value)) {
        TValue key;
        setsvalue(L, &key, s);
        luaH_finishset(L, seen, &key, value, &key);
        luaC_barrierback(L, obj2gco(seen), &key);
        return s;
    }
    TString *shared = tsvalue(value);
    luaC_objbarrier(L, owner, shared);
    return shared;
}
static void share_literals(lua_State *L, Table *seen, Proto *p) {
    for (int i = 0; i < p->sizek; ++i) {
        TValue *v = &p->k[i];
        if (ttislngstring(v)) {
            TString *s = canonical(L, seen, p, tsvalue(v));
            setsvalue(L, v, s);
        }
    }
    for (int i = 0; i < p->sizelocvars; ++i)
        p->locvars[i].varname = canonical(L, seen, p, p->locvars[i].varname);
    for (int i = 0; i < p->sizeupvalues; ++i)
        p->upvalues[i].name = canonical(L, seen, p, p->upvalues[i].name);
    for (int i = 0; i < p->sizep; ++i)
        share_literals(L, seen, p->p[i]);
}
/* luaX_newstring shares long literals within a translation unit. lua_dump
   serializes each occurrence independently. Restore the source-level identity
   here, before exposing the closure to Lua, with normal write barriers. */
LUA_API int gpt_load_compiled(lua_State *L) {
    size_t size;
    const char *bytes = luaL_checklstring(L, 1, &size);
    const char *name = luaL_checkstring(L, 2);
    int has_env = lua_gettop(L) >= 3;
    if (luaL_loadbufferx(L, bytes, size, name, "b") != LUA_OK)
        return lua_error(L);
    if (has_env) {
        lua_pushvalue(L, 3);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    Proto *p = clLvalue(s2v(L->top.p - 1))->p;
    lua_newtable(L);
    share_literals(L, hvalue(s2v(L->top.p - 1)), p);
    lua_pop(L, 1);
    return 1;
}
