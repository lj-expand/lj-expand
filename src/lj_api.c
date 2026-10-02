/*
** Public Lua/C API.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_api_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_expand_lib.h"
#include "lj_expand_log.h"
#include "lj_expand_module.h"
#include "lj_expand_detour.h"
#include "lj_expand_platform.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lj_dispatch.h"
#include "lj_expand_globals.h"
#include "lj_expand_script.h"
#include "lj_expand_script_watcher.h"
#include "lj_record.h"
#include "lj_parse.h"
#include "lauxlib.h"
#include "lj_expand_startup.h"
#include "lj_lib.h"
#include "lj_ffrecord.h"
#include "stdio.h"
#include "lj_buf.h"
#include "lj_expand_cmd.h"
#include "lj_expand_crash_handler.h"
#include "lj_expand_dirs.h"
#include "lj_expand_isolation.h"
#include "lj_expand_path.h"
#include "lj_expand_proxy.h"
#include "lj_expand_log.h"
#include "lj_expand_settings.h"


/* -- Common helper functions --------------------------------------------- */

#define api_checknelems(L, n)		api_check(L, (n) <= (L->top - L->base))
#define api_checkvalidindex(L, i)	api_check(L, (i) != niltv(L))
#define lje_redirect_state(L) \
  if (LJEG()->redirect_to_isolation) { \
    L = LJEG()->isolated_state; \
  }

static TValue *index2adr(lua_State *L, int idx)
{
  if (idx > 0) {
    TValue *o = L->base + (idx - 1);
    return o < L->top ? o : niltv(L);
  } else if (idx > LUA_REGISTRYINDEX) {
    api_check(L, idx != 0 && -idx <= L->top - L->base);
    return L->top + idx;
  } else if (idx == LUA_GLOBALSINDEX) {
    TValue *o = &G(L)->tmptv;
    settabV(L, o, tabref(L->env));
    return o;
  } else if (idx == LUA_REGISTRYINDEX) {
    if (LJEG()->redirect_to_isolation)
    {
      TValue* o = &G(L)->tmptv;
      settabV(L, o, LJE_SHADOW());
      return o;
    }

    return registry(L);
  } else {
    GCfunc *fn = curr_func(L);
    api_check(L, fn->c.gct == ~LJ_TFUNC && !isluafunc(fn));
    if (idx == LUA_ENVIRONINDEX) {
      TValue *o = &G(L)->tmptv;
      settabV(L, o, tabref(fn->c.env));
      return o;
    } else {
      idx = LUA_GLOBALSINDEX - idx;
      return idx <= fn->c.nupvalues ? &fn->c.upvalue[idx-1] : niltv(L);
    }
  }
}

static TValue *stkindex2adr(lua_State *L, int idx)
{
  if (idx > 0) {
    TValue *o = L->base + (idx - 1);
    return o < L->top ? o : niltv(L);
  } else {
    api_check(L, idx != 0 && -idx <= L->top - L->base);
    return L->top + idx;
  }
}

static GCtab *getcurrenv(lua_State *L)
{
  GCfunc *fn = curr_func(L);
  return fn->c.gct == ~LJ_TFUNC ? tabref(fn->c.env) : tabref(L->env);
}

/* -- Miscellaneous API functions ----------------------------------------- */

LUA_API int lua_status(lua_State *L)
{
  return L->status;
}

LUA_API int lua_checkstack(lua_State *L, int size)
{
  lje_redirect_state(L);
  if (size > LUAI_MAXCSTACK || (L->top - L->base + size) > LUAI_MAXCSTACK) {
    return 0;  /* Stack overflow. */
  } else if (size > 0) {
    lj_state_checkstack(L, (MSize)size);
  }
  return 1;
}

LUALIB_API void luaL_checkstack(lua_State *L, int size, const char *msg)
{
  lje_redirect_state(L);
  if (!lua_checkstack(L, size))
    lj_err_callerv(L, LJ_ERR_STKOVM, msg);
}

LUA_API void lua_xmove(lua_State *from, lua_State *to, int n)
{
  TValue *f, *t;
  if (from == to) return;
  api_checknelems(from, n);
  api_check(from, G(from) == G(to));
  lj_state_checkstack(to, (MSize)n);
  f = from->top;
  t = to->top = to->top + n;
  while (--n >= 0) copyTV(to, --t, --f);
  from->top = f;
}

LUA_API const lua_Number *lua_version(lua_State *L)
{
  static const lua_Number version = LUA_VERSION_NUM;
  UNUSED(L);
  return &version;
}

/* -- Stack manipulation -------------------------------------------------- */

LUA_API int lua_gettop(lua_State *L)
{
  lje_redirect_state(L);
  return (int)(L->top - L->base);
}

LUA_API void lua_settop(lua_State *L, int idx)
{
  lje_redirect_state(L);
  if (idx >= 0) {
    api_check(L, idx <= tvref(L->maxstack) - L->base);
    if (L->base + idx > L->top) {
      if (L->base + idx >= tvref(L->maxstack))
	lj_state_growstack(L, (MSize)idx - (MSize)(L->top - L->base));
      do { setnilV(L->top++); } while (L->top < L->base + idx);
    } else {
      L->top = L->base + idx;
    }
  } else {
    api_check(L, -(idx+1) <= (L->top - L->base));
    L->top += idx+1;  /* Shrinks top (idx < 0). */
  }
}

LUA_API void lua_remove(lua_State *L, int idx)
{
  lje_redirect_state(L);
  TValue *p = stkindex2adr(L, idx);
  if (p == niltv(L))
  {
    LJE_WARN("Invalid index passed to lua_remove: %d", idx);
    return;
  }

  api_checkvalidindex(L, p);
  while (++p < L->top) copyTV(L, p-1, p);
  L->top--;
}

LUA_API void lua_insert(lua_State *L, int idx)
{
  lje_redirect_state(L);
  TValue *q, *p = stkindex2adr(L, idx);
  api_checkvalidindex(L, p);
  for (q = L->top; q > p; q--) copyTV(L, q, q-1);
  copyTV(L, p, L->top);
}

static void copy_slot(lua_State *L, TValue *f, int idx)
{
  if (idx == LUA_GLOBALSINDEX) {
    api_check(L, tvistab(f));
    /* NOBARRIER: A thread (i.e. L) is never black. */
    setgcref(L->env, obj2gco(tabV(f)));
  } else if (idx == LUA_ENVIRONINDEX) {
    GCfunc *fn = curr_func(L);
    if (fn->c.gct != ~LJ_TFUNC)
      lj_err_msg(L, LJ_ERR_NOENV);
    api_check(L, tvistab(f));
    setgcref(fn->c.env, obj2gco(tabV(f)));
    lj_gc_barrier(L, fn, f);
  } else {
    TValue *o = index2adr(L, idx);
    api_checkvalidindex(L, o);
    copyTV(L, o, f);
    if (idx < LUA_GLOBALSINDEX)  /* Need a barrier for upvalues. */
      lj_gc_barrier(L, curr_func(L), f);
  }
}

LUA_API void lua_replace(lua_State *L, int idx)
{
  lje_redirect_state(L);
  api_checknelems(L, 1);
  copy_slot(L, L->top - 1, idx);
  L->top--;
}

LUA_API void lua_copy(lua_State *L, int fromidx, int toidx)
{
  lje_redirect_state(L);
  copy_slot(L, index2adr(L, fromidx), toidx);
}

LUA_API void lua_pushvalue(lua_State *L, int idx)
{
  lje_redirect_state(L);
  copyTV(L, L->top, index2adr(L, idx));
  incr_top(L);
}

/* -- Stack getters ------------------------------------------------------- */

LUA_API int lua_type(lua_State *L, int idx)
{
  lje_redirect_state(L);

  cTValue *o = index2adr(L, idx);
  if (tvisnumber(o)) {
    return LUA_TNUMBER;
#if LJ_64 && !LJ_GC64
  } else if (tvislightud(o)) {
    return LUA_TLIGHTUSERDATA;
#endif
  } else if (o == niltv(L)) {
    return LUA_TNONE;
  } else {  /* Magic internal/external tag conversion. ORDER LJ_T */
    uint32_t t = ~itype(o);
#if LJ_64
    int tt = (int)((U64x(75a06,98042110) >> 4*t) & 15u);
#else
    int tt = (int)(((t < 8 ? 0x98042110u : 0x75a06u) >> 4*(t&7)) & 15u);
#endif
    lua_assert(tt != LUA_TNIL || tvisnil(o));
    return tt;
  }
}

LUALIB_API void luaL_checktype(lua_State *L, int idx, int tt)
{
  lje_redirect_state(L);
  if (lua_type(L, idx) != tt)
    lj_err_argt(L, idx, tt);
}

LUALIB_API void luaL_checkany(lua_State *L, int idx)
{
  if (index2adr(L, idx) == niltv(L))
    lj_err_arg(L, idx, LJ_ERR_NOVAL);
}

LUA_API const char *lua_typename(lua_State *L, int t)
{
  UNUSED(L);
  return lj_obj_typename[t+1];
}

LUA_API int lua_iscfunction(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  return tvisfunc(o) && !isluafunc(funcV(o));
}

LUA_API int lua_isnumber(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  return (tvisnumber(o) || (tvisstr(o) && lj_strscan_number(strV(o), &tmp)));
}

LUA_API int lua_isstring(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  return (tvisstr(o) || tvisnumber(o));
}

LUA_API int lua_isuserdata(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  return (tvisudata(o) || tvislightud(o));
}

LUA_API int lua_rawequal(lua_State *L, int idx1, int idx2)
{
  lje_redirect_state(L);
  cTValue *o1 = index2adr(L, idx1);
  cTValue *o2 = index2adr(L, idx2);
  return (o1 == niltv(L) || o2 == niltv(L)) ? 0 : lj_obj_equal(o1, o2);
}

LUA_API int lua_equal(lua_State *L, int idx1, int idx2)
{
  lje_redirect_state(L);
  cTValue *o1 = index2adr(L, idx1);
  cTValue *o2 = index2adr(L, idx2);
  if (tvisint(o1) && tvisint(o2)) {
    return intV(o1) == intV(o2);
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    return numberVnum(o1) == numberVnum(o2);
  } else if (itype(o1) != itype(o2)) {
    return 0;
  } else if (tvispri(o1)) {
    return o1 != niltv(L) && o2 != niltv(L);
#if LJ_64 && !LJ_GC64
  } else if (tvislightud(o1)) {
    return o1->u64 == o2->u64;
#endif
  } else if (gcrefeq(o1->gcr, o2->gcr)) {
    return 1;
  } else if (!tvistabud(o1)) {
    return 0;
  } else {
    TValue *base = lj_meta_equal(L, gcV(o1), gcV(o2), 0);
    if ((uintptr_t)base <= 1) {
      return (int)(uintptr_t)base;
    } else {
      L->top = base+2;
      lj_vm_call(L, base, 1+1);
      L->top -= 2+LJ_FR2;
      return tvistruecond(L->top+1+LJ_FR2);
    }
  }
}

LUA_API int lua_lessthan(lua_State *L, int idx1, int idx2)
{
  lje_redirect_state(L);
  cTValue *o1 = index2adr(L, idx1);
  cTValue *o2 = index2adr(L, idx2);
  if (o1 == niltv(L) || o2 == niltv(L)) {
    return 0;
  } else if (tvisint(o1) && tvisint(o2)) {
    return intV(o1) < intV(o2);
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    return numberVnum(o1) < numberVnum(o2);
  } else {
    TValue *base = lj_meta_comp(L, o1, o2, 0);
    if ((uintptr_t)base <= 1) {
      return (int)(uintptr_t)base;
    } else {
      L->top = base+2;
      lj_vm_call(L, base, 1+1);
      L->top -= 2+LJ_FR2;
      return tvistruecond(L->top+1+LJ_FR2);
    }
  }
}

LUA_API lua_Number lua_tonumber(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  if (LJ_LIKELY(tvisnumber(o)))
    return numberVnum(o);
  else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp))
    return numV(&tmp);
  else
    return 0;
}

LUA_API lua_Number lua_tonumberx(lua_State *L, int idx, int *ok)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  if (LJ_LIKELY(tvisnumber(o))) {
    if (ok) *ok = 1;
    return numberVnum(o);
  } else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp)) {
    if (ok) *ok = 1;
    return numV(&tmp);
  } else {
    if (ok) *ok = 0;
    return 0;
  }
}

LUALIB_API lua_Number luaL_checknumber(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  if (LJ_LIKELY(tvisnumber(o)))
    return numberVnum(o);
  else if (!(tvisstr(o) && lj_strscan_num(strV(o), &tmp)))
    lj_err_argt(L, idx, LUA_TNUMBER);
  return numV(&tmp);
}

LUALIB_API lua_Number luaL_optnumber(lua_State *L, int idx, lua_Number def)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  if (LJ_LIKELY(tvisnumber(o)))
    return numberVnum(o);
  else if (tvisnil(o))
    return def;
  else if (!(tvisstr(o) && lj_strscan_num(strV(o), &tmp)))
    lj_err_argt(L, idx, LUA_TNUMBER);
  return numV(&tmp);
}

LUA_API lua_Integer lua_tointeger(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  lua_Number n;
  if (LJ_LIKELY(tvisint(o))) {
    return intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp)))
      return 0;
    if (tvisint(&tmp))
      return intV(&tmp);
    n = numV(&tmp);
  }
#if LJ_64
  return (lua_Integer)n;
#else
  return lj_num2int(n);
#endif
}

LUA_API lua_Integer lua_tointegerx(lua_State *L, int idx, int *ok)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  lua_Number n;
  if (LJ_LIKELY(tvisint(o))) {
    if (ok) *ok = 1;
    return intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp))) {
      if (ok) *ok = 0;
      return 0;
    }
    if (tvisint(&tmp)) {
      if (ok) *ok = 1;
      return intV(&tmp);
    }
    n = numV(&tmp);
  }
  if (ok) *ok = 1;
#if LJ_64
  return (lua_Integer)n;
#else
  return lj_num2int(n);
#endif
}

LUALIB_API lua_Integer luaL_checkinteger(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  lua_Number n;
  if (LJ_LIKELY(tvisint(o))) {
    return intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp)))
      lj_err_argt(L, idx, LUA_TNUMBER);
    if (tvisint(&tmp))
      return (lua_Integer)intV(&tmp);
    n = numV(&tmp);
  }
#if LJ_64
  return (lua_Integer)n;
#else
  return lj_num2int(n);
#endif
}

LUALIB_API lua_Integer luaL_optinteger(lua_State *L, int idx, lua_Integer def)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  TValue tmp;
  lua_Number n;
  if (LJ_LIKELY(tvisint(o))) {
    return intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
  } else if (tvisnil(o)) {
    return def;
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp)))
      lj_err_argt(L, idx, LUA_TNUMBER);
    if (tvisint(&tmp))
      return (lua_Integer)intV(&tmp);
    n = numV(&tmp);
  }
#if LJ_64
  return (lua_Integer)n;
#else
  return lj_num2int(n);
#endif
}

LUA_API int lua_toboolean(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  return tvistruecond(o);
}

LUA_API const char *lua_tolstring(lua_State *L, int idx, size_t *len)
{
  lje_redirect_state(L);
  TValue *o = index2adr(L, idx);
  GCstr *s;
  if (LJ_LIKELY(tvisstr(o))) {
    s = strV(o);
  } else if (tvisnumber(o)) {
    lj_gc_check(L);
    o = index2adr(L, idx);  /* GC may move the stack. */
    s = lj_strfmt_number(L, o);
    setstrV(L, o, s);
  } else {
    if (len != NULL) *len = 0;
    return NULL;
  }
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API const char *luaL_checklstring(lua_State *L, int idx, size_t *len)
{
  lje_redirect_state(L);
  TValue *o = index2adr(L, idx);
  GCstr *s;
  if (LJ_LIKELY(tvisstr(o))) {
    s = strV(o);
  } else if (tvisnumber(o)) {
    lj_gc_check(L);
    o = index2adr(L, idx);  /* GC may move the stack. */
    s = lj_strfmt_number(L, o);
    setstrV(L, o, s);
  } else {
    lj_err_argt(L, idx, LUA_TSTRING);
  }
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API const char *luaL_optlstring(lua_State *L, int idx,
				       const char *def, size_t *len)
{
  lje_redirect_state(L);
  TValue *o = index2adr(L, idx);
  GCstr *s;
  if (LJ_LIKELY(tvisstr(o))) {
    s = strV(o);
  } else if (tvisnil(o)) {
    if (len != NULL) *len = def ? strlen(def) : 0;
    return def;
  } else if (tvisnumber(o)) {
    lj_gc_check(L);
    o = index2adr(L, idx);  /* GC may move the stack. */
    s = lj_strfmt_number(L, o);
    setstrV(L, o, s);
  } else {
    lj_err_argt(L, idx, LUA_TSTRING);
  }
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API int luaL_checkoption(lua_State *L, int idx, const char *def,
				const char *const lst[])
{
  lje_redirect_state(L);
  ptrdiff_t i;
  const char *s = lua_tolstring(L, idx, NULL);
  if (s == NULL && (s = def) == NULL)
    lj_err_argt(L, idx, LUA_TSTRING);
  for (i = 0; lst[i]; i++)
    if (strcmp(lst[i], s) == 0)
      return (int)i;
  lj_err_argv(L, idx, LJ_ERR_INVOPTM, s);
}

LUA_API size_t lua_objlen(lua_State *L, int idx)
{
  lje_redirect_state(L);
  TValue *o = index2adr(L, idx);
  if (tvisstr(o)) {
    return strV(o)->len;
  } else if (tvistab(o)) {
    return (size_t)lj_tab_len(tabV(o));
  } else if (tvisudata(o)) {
    return udataV(o)->len;
  } else if (tvisnumber(o)) {
    GCstr *s = lj_strfmt_number(L, o);
    setstrV(L, o, s);
    return s->len;
  } else {
    return 0;
  }
}

LUA_API lua_CFunction lua_tocfunction(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  if (tvisfunc(o)) {
    BCOp op = bc_op(*mref(funcV(o)->c.pc, BCIns));
    if (op == BC_FUNCC || op == BC_FUNCCW)
      return funcV(o)->c.f;
  }
  return NULL;
}

LUA_API void *lua_touserdata(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  if (tvisudata(o))
    return uddata(udataV(o));
  else if (tvislightud(o))
    return lightudV(o);
  else
    return NULL;
}

LUA_API lua_State *lua_tothread(lua_State *L, int idx)
{
  cTValue *o = index2adr(L, idx);
  return (!tvisthread(o)) ? NULL : threadV(o);
}

LUA_API const void *lua_topointer(lua_State *L, int idx)
{
  return lj_obj_ptr(index2adr(L, idx));
}

/* -- Stack setters (object creation) ------------------------------------- */

LUA_API void lua_pushnil(lua_State *L)
{
  lje_redirect_state(L);
  setnilV(L->top);
  incr_top(L);
}

LUA_API void lua_pushnumber(lua_State *L, lua_Number n)
{
  lje_redirect_state(L);
  setnumV(L->top, n);
  if (LJ_UNLIKELY(tvisnan(L->top)))
    setnanV(L->top);  /* Canonicalize injected NaNs. */
  incr_top(L);
}

LUA_API void lua_pushinteger(lua_State *L, lua_Integer n)
{
  lje_redirect_state(L);
  setintptrV(L->top, n);
  incr_top(L);
}

LUA_API void lua_pushlstring(lua_State *L, const char *str, size_t len)
{
  lje_redirect_state(L);
  GCstr *s;
  lj_gc_check(L);
  s = lj_str_new(L, str, len);
  setstrV(L, L->top, s);
  incr_top(L);
}

LUA_API void lua_pushstring(lua_State *L, const char *str)
{
  lje_redirect_state(L);
  if (str == NULL) {
    setnilV(L->top);
  } else {
    GCstr *s;
    lj_gc_check(L);
    s = lj_str_newz(L, str);
    setstrV(L, L->top, s);
  }
  incr_top(L);
}

LUA_API const char *lua_pushvfstring(lua_State *L, const char *fmt,
				     va_list argp)
{
  lje_redirect_state(L);
  lj_gc_check(L);
  return lj_strfmt_pushvf(L, fmt, argp);
}

LUA_API const char *lua_pushfstring(lua_State *L, const char *fmt, ...)
{
  lje_redirect_state(L);
  const char *ret;
  va_list argp;
  lj_gc_check(L);
  va_start(argp, fmt);
  ret = lj_strfmt_pushvf(L, fmt, argp);
  va_end(argp);
  return ret;
}

LUA_API void lua_pushcclosure(lua_State *L, lua_CFunction f, int n)
{
  lje_redirect_state(L);
  GCfunc *fn;
  lj_gc_check(L);
  api_checknelems(L, n);
  fn = lj_func_newC(L, (MSize)n, getcurrenv(L));
  fn->c.f = f;
  L->top -= n;
  while (n--)
    copyTV(L, &fn->c.upvalue[n], L->top+n);
  setfuncV(L, L->top, fn);
  lua_assert(iswhite(obj2gco(fn)));
  incr_top(L);
}

LUA_API void lua_pushboolean(lua_State *L, int b)
{
  lje_redirect_state(L);
  setboolV(L->top, (b != 0));
  incr_top(L);
}

LUA_API void lua_pushlightuserdata(lua_State *L, void *p)
{
  lje_redirect_state(L);
  setlightudV(L->top, checklightudptr(L, p));
  incr_top(L);
}

LUA_API void lua_createtable(lua_State *L, int narray, int nrec)
{
  lje_redirect_state(L);
  lj_gc_check(L);
  settabV(L, L->top, lj_tab_new_ah(L, narray, nrec));
  incr_top(L);
}

LUALIB_API int luaL_newmetatable(lua_State *L, const char *tname)
{
  lje_redirect_state(L);
  GCtab *regt = tabV(registry(L));
  if (LJEG()->redirect_to_isolation)
  {
    regt = LJE_SHADOW();
  }
  TValue *tv = lj_tab_setstr(L, regt, lj_str_newz(L, tname));
  if (tvisnil(tv)) {
    GCtab *mt = lj_tab_new(L, 0, 1);
    settabV(L, tv, mt);
    settabV(L, L->top++, mt);
    lj_gc_anybarriert(L, regt);
    return 1;
  } else {
    copyTV(L, L->top++, tv);
    return 0;
  }
}

LUA_API int lua_pushthread(lua_State *L)
{
  lje_redirect_state(L);
  setthreadV(L, L->top, L);
  incr_top(L);
  return (mainthread(G(L)) == L);
}

LUA_API lua_State *lua_newthread(lua_State *L)
{
  lua_State *L1;
  lj_gc_check(L);
  L1 = lj_state_new(L);
  setthreadV(L, L->top, L1);
  incr_top(L);
  return L1;
}

LUA_API void *lua_newuserdata(lua_State *L, size_t size)
{
  lje_redirect_state(L);
  GCudata *ud;
  lj_gc_check(L);
  if (size > LJ_MAX_UDATA)
    lj_err_msg(L, LJ_ERR_UDATAOV);
  ud = lj_udata_new(L, (MSize)size, getcurrenv(L));
  setudataV(L, L->top, ud);
  incr_top(L);
  return uddata(ud);
}

LUA_API void lua_concat(lua_State *L, int n)
{
  lje_redirect_state(L);
  api_checknelems(L, n);
  if (n >= 2) {
    n--;
    do {
      TValue *top = lj_meta_cat(L, L->top-1, -n);
      if (top == NULL) {
	L->top -= n;
	break;
      }
      n -= (int)(L->top - top);
      L->top = top+2;
      lj_vm_call(L, top, 1+1);
      L->top -= 1+LJ_FR2;
      copyTV(L, L->top-1, L->top+LJ_FR2);
    } while (--n > 0);
  } else if (n == 0) {  /* Push empty string. */
    setstrV(L, L->top, &G(L)->strempty);
    incr_top(L);
  }
  /* else n == 1: nothing to do. */
}

/* -- Object getters ------------------------------------------------------ */

LUA_API void lua_gettable(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *v, *t = index2adr(L, idx);
  api_checkvalidindex(L, t);
  v = lj_meta_tget(L, t, L->top-1);
  if (v == NULL) {
    L->top += 2;
    lj_vm_call(L, L->top-2, 1+1);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  copyTV(L, L->top-1, v);
}

LUA_API void lua_getfield(lua_State *L, int idx, const char *k)
{
  lje_redirect_state(L);

  cTValue *v, *t = index2adr(L, idx);
  TValue key;
  api_checkvalidindex(L, t);
  setstrV(L, &key, lj_str_newz(L, k));
  v = lj_meta_tget(L, t, &key);
  if (v == NULL) {
    L->top += 2;
    lj_vm_call(L, L->top-2, 1+1);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  copyTV(L, L->top, v);
  incr_top(L);
}

LUA_API void lua_rawget(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *t = index2adr(L, idx);
  api_check(L, tvistab(t));
  copyTV(L, L->top-1, lj_tab_get(L, tabV(t), L->top-1));
}

static void lje_dump_stack(lua_State* L);
LUA_API void lua_rawgeti(lua_State *L, int idx, int n)
{
  lje_redirect_state(L);

  TValue shadow_registry;
  settabV(L, &shadow_registry, LJE_SHADOW());

  /* Hoisted: the miss path below is also reached by goto, which would skip an
     initialiser declared inside it. */
  lua_State* redirect_host_state = lje_host_state(LJEG()->redirect_host);

  cTValue *v, *t = index2adr(L, idx);
  if (LJEG()->redirect_to_isolation && idx == LUA_REGISTRYINDEX)
  {
    t = &shadow_registry;
  }

  api_check(L, tvistab(t));
  v = lj_tab_getint(tabV(t), n);

  if (v) {
    if (LJEG()->redirect_to_isolation && idx == LUA_REGISTRYINDEX && tvisnil(v))
    {
      goto copy_to_isolated_registry; /* Means we invalidated this from the preinit Lua script. Fetch it again from host. */
    }
    /* LJE: Tag registry-backed host userdata with its ref index (align1 is dead
     * padding) so engine call hooks can reuse the shadow registry copy later. */
    if (!LJEG()->redirect_to_isolation && idx == LUA_REGISTRYINDEX &&
        lje_host_id_of(L, NULL) && n != 0 && tvisudata(v))
      udataV(v)->align1 = (uint32_t)n;
    copyTV(L, L->top, v);
  } else {
    if (LJEG()->redirect_to_isolation && idx == LUA_REGISTRYINDEX)
    {
copy_to_isolated_registry:
      /* Slot 0 == freelist head. Will cause collisions. */
      if (n != 0 && redirect_host_state)
      {
        lua_State* host = redirect_host_state;
        cTValue* reg = registry(host);
        v = lj_tab_getint(tabV(reg), n);
        if (v && !tvisnil(v))
        {
          if (tvistab(v))
          {
            // GMod metatables carry a MetaName. If our state already registered its own
            // version, redirect to it instead of deep-copying the host's table.
            cTValue* meta_name = lj_tab_getstr_lit(tabV(v), "MetaName");
            if (meta_name && tvisstr(meta_name))
            {
              const char* name = strdata(strV(meta_name));
              GCtab* to_reg = tabV(registry(L)); /* not shadow, metatables are stored in main */
              cTValue* to_metatable = lj_tab_getstr_raw(to_reg, name, strlen(name)); // Can't use raw GCstr, comes from the host state
              if (to_metatable && tvistab(to_metatable))
              {
                copyTV(L, L->top, to_metatable);
                incr_top(L);

                /* Cache it in the shadow registry under the same ref so the next
                 * lookup hits the fast path without re-checking the host. */
                GCtab* isolated_reg = LJE_SHADOW();
                TValue* dst = lj_tab_setint(L, isolated_reg, n);
                copyTV(L, dst, to_metatable);
                lj_gc_barriert(L, isolated_reg, dst);
                return;
              }
            }
          }

          lje_copy_to_isolated_state_tv(host, L, v, 0);
          L->top--;
          incr_top(L);

          /* Copying each time isn't good though. Save it in our registry so the
           * next lookup succeeds without copying. No numbers since those are freelist links. */
          if (!tvisnumber(v))
          {
            GCtab* isolated_reg = LJE_SHADOW();
            TValue* dst = lj_tab_setint(L, isolated_reg, n);
            TValue* src = L->top - 1; /* what we just pushed */
            copyTV(L, dst, src);
            lj_gc_barriert(L, isolated_reg, dst);
          }
          return;
        }
      }
    }
    setnilV(L->top);
  }
  incr_top(L);
}

LUA_API int lua_getmetatable(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  GCtab *mt = NULL;
  if (tvistab(o))
    mt = tabref(tabV(o)->metatable);
  else if (tvisudata(o))
    mt = tabref(udataV(o)->metatable);
  else
    mt = tabref(basemt_obj(G(L), o));
  if (mt == NULL)
    return 0;
  settabV(L, L->top, mt);
  incr_top(L);
  return 1;
}

LUALIB_API int luaL_getmetafield(lua_State *L, int idx, const char *field)
{
  if (lua_getmetatable(L, idx)) {
    cTValue *tv = lj_tab_getstr(tabV(L->top-1), lj_str_newz(L, field));
    if (tv && !tvisnil(tv)) {
      copyTV(L, L->top-1, tv);
      return 1;
    }
    L->top--;
  }
  return 0;
}

LUA_API void lua_getfenv(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  api_checkvalidindex(L, o);
  if (tvisfunc(o)) {
    settabV(L, L->top, tabref(funcV(o)->c.env));
  } else if (tvisudata(o)) {
    settabV(L, L->top, tabref(udataV(o)->env));
  } else if (tvisthread(o)) {
    settabV(L, L->top, tabref(threadV(o)->env));
  } else {
    setnilV(L->top);
  }
  incr_top(L);
}

LUA_API int lua_next(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *t = index2adr(L, idx);
  int more;
  api_check(L, tvistab(t));
  more = lj_tab_next(L, tabV(t), L->top-1);
  if (more) {
    incr_top(L);  /* Return new key and value slot. */
  } else {  /* End of traversal. */
    L->top--;  /* Remove key slot. */
  }
  return more;
}

LUA_API const char *lua_getupvalue(lua_State *L, int idx, int n)
{
  TValue *val;
  const char *name = lj_debug_uvnamev(index2adr(L, idx), (uint32_t)(n-1), &val);
  if (name) {
    copyTV(L, L->top, val);
    incr_top(L);
  }
  return name;
}

LUA_API void *lua_upvalueid(lua_State *L, int idx, int n)
{
  GCfunc *fn = funcV(index2adr(L, idx));
  n--;
  api_check(L, (uint32_t)n < fn->l.nupvalues);
  return isluafunc(fn) ? (void *)gcref(fn->l.uvptr[n]) :
			 (void *)&fn->c.upvalue[n];
}

LUA_API void lua_upvaluejoin(lua_State *L, int idx1, int n1, int idx2, int n2)
{
  lje_redirect_state(L);
  GCfunc *fn1 = funcV(index2adr(L, idx1));
  GCfunc *fn2 = funcV(index2adr(L, idx2));
  n1--; n2--;
  api_check(L, isluafunc(fn1) && (uint32_t)n1 < fn1->l.nupvalues);
  api_check(L, isluafunc(fn2) && (uint32_t)n2 < fn2->l.nupvalues);
  setgcrefr(fn1->l.uvptr[n1], fn2->l.uvptr[n2]);
  lj_gc_objbarrier(L, fn1, gcref(fn1->l.uvptr[n1]));
}

LUALIB_API void *luaL_testudata(lua_State *L, int idx, const char *tname)
{
  cTValue *o = index2adr(L, idx);
  if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    cTValue *tv = lj_tab_getstr(tabV(registry(L)), lj_str_newz(L, tname));
    if (tv && tvistab(tv) && tabV(tv) == tabref(ud->metatable))
      return uddata(ud);
  }
  return NULL;  /* value is not a userdata with a metatable */
}

LUALIB_API void *luaL_checkudata(lua_State *L, int idx, const char *tname)
{
  void *p = luaL_testudata(L, idx, tname);
  if (!p) lj_err_argtype(L, idx, tname);
  return p;
}

/* -- Object setters ------------------------------------------------------ */

LUA_API void lua_settable(lua_State *L, int idx)
{
  lje_redirect_state(L);
  TValue *o;
  cTValue *t = index2adr(L, idx);
  api_checknelems(L, 2);
  api_checkvalidindex(L, t);
  o = lj_meta_tset(L, t, L->top-2);
  if (o) {
    /* NOBARRIER: lj_meta_tset ensures the table is not black. */
    L->top -= 2;
    copyTV(L, o, L->top+1);
  } else {
    TValue *base = L->top;
    copyTV(L, base+2, base-3-2*LJ_FR2);
    L->top = base+3;
    lj_vm_call(L, base, 0+1);
    L->top -= 3+LJ_FR2;
  }
}

LUA_API void lua_setfield(lua_State *L, int idx, const char *k)
{
  lje_redirect_state(L);
  TValue *o;
  TValue key;
  cTValue *t = index2adr(L, idx);
  api_checknelems(L, 1);
  api_checkvalidindex(L, t);
  setstrV(L, &key, lj_str_newz(L, k));
  o = lj_meta_tset(L, t, &key);
  if (o) {
    /* NOBARRIER: lj_meta_tset ensures the table is not black. */
    copyTV(L, o, --L->top);
  } else {
    TValue *base = L->top;
    copyTV(L, base+2, base-3-2*LJ_FR2);
    L->top = base+3;
    lj_vm_call(L, base, 0+1);
    L->top -= 2+LJ_FR2;
  }
}

LUA_API void lua_rawset(lua_State *L, int idx)
{
  lje_redirect_state(L);
  GCtab *t = tabV(index2adr(L, idx));
  TValue *dst, *key;
  api_checknelems(L, 2);
  key = L->top-2;
  dst = lj_tab_set(L, t, key);
  copyTV(L, dst, key+1);
  lj_gc_anybarriert(L, t);
  L->top = key;
}

LUA_API void lua_rawseti(lua_State *L, int idx, int n)
{
  lje_redirect_state(L);

  GCtab *t = tabV(index2adr(L, idx));
  TValue *dst, *src;
  api_checknelems(L, 1);
  dst = lj_tab_setint(L, t, n);
  src = L->top-1;
  copyTV(L, dst, src);
  lj_gc_barriert(L, t, dst);
  L->top = src;

  /* LJE: Clear any host registry writes in that host's shadow registry so they are identical again. */
  LJEHostId host = LJE_HOST_CLIENT;
  if (!LJEG()->redirect_to_isolation && idx == LUA_REGISTRYINDEX &&
      lje_host_id_of(L, &host) && lje_host_view(host)->shadow_registry && n != 0) {
    TValue *cached = (TValue *)lj_tab_getint(lje_host_view(host)->shadow_registry, n);
    if (cached) setnilV(cached);
    if (tvisudata(dst)) {
      // Tag this userdata with its registry index for caching
      udataV(dst)->align1 = (uint32_t)n;
    }
  }
}

LUA_API int lua_setmetatable(lua_State *L, int idx)
{
  lje_redirect_state(L);
  global_State *g;
  GCtab *mt;
  cTValue *o = index2adr(L, idx);
  api_checknelems(L, 1);
  api_checkvalidindex(L, o);
  if (tvisnil(L->top-1)) {
    mt = NULL;
  } else {
    api_check(L, tvistab(L->top-1));
    mt = tabV(L->top-1);
  }
  g = G(L);
  if (tvistab(o)) {
    setgcref(tabV(o)->metatable, obj2gco(mt));
    if (mt)
      lj_gc_objbarriert(L, tabV(o), mt);
  } else if (tvisudata(o)) {
    setgcref(udataV(o)->metatable, obj2gco(mt));
    if (mt)
      lj_gc_objbarrier(L, udataV(o), mt);
  } else {
    /* Flush cache, since traces specialize to basemt. But not during __gc. */
    if (lj_trace_flushall(L))
      lj_err_caller(L, LJ_ERR_NOGCMM);
    if (tvisbool(o)) {
      /* NOBARRIER: basemt is a GC root. */
      setgcref(basemt_it(g, LJ_TTRUE), obj2gco(mt));
      setgcref(basemt_it(g, LJ_TFALSE), obj2gco(mt));
    } else {
      /* NOBARRIER: basemt is a GC root. */
      setgcref(basemt_obj(g, o), obj2gco(mt));
    }
  }
  L->top--;
  return 1;
}

LUALIB_API void luaL_setmetatable(lua_State *L, const char *tname)
{
  lua_getfield(L, LUA_REGISTRYINDEX, tname);
  lua_setmetatable(L, -2);
}

LUA_API int lua_setfenv(lua_State *L, int idx)
{
  lje_redirect_state(L);
  cTValue *o = index2adr(L, idx);
  GCtab *t;
  api_checknelems(L, 1);
  api_checkvalidindex(L, o);
  api_check(L, tvistab(L->top-1));
  t = tabV(L->top-1);
  if (tvisfunc(o)) {
    setgcref(funcV(o)->c.env, obj2gco(t));
  } else if (tvisudata(o)) {
    setgcref(udataV(o)->env, obj2gco(t));
  } else if (tvisthread(o)) {
    setgcref(threadV(o)->env, obj2gco(t));
  } else {
    L->top--;
    return 0;
  }
  lj_gc_objbarrier(L, gcV(o), t);
  L->top--;
  return 1;
}

LUA_API const char *lua_setupvalue(lua_State *L, int idx, int n)
{
  cTValue *f = index2adr(L, idx);
  TValue *val;
  const char *name;
  api_checknelems(L, 1);
  name = lj_debug_uvnamev(f, (uint32_t)(n-1), &val);
  if (name) {
    L->top--;
    copyTV(L, val, L->top);
    lj_gc_barrier(L, funcV(f), L->top);
  }
  return name;
}

/* -- Calls --------------------------------------------------------------- */

#if LJ_FR2
static TValue *api_call_base(lua_State *L, int nargs)
{
  TValue *o = L->top, *base = o - nargs;
  L->top = o+1;
  for (; o > base; o--) copyTV(L, o, o-1);
  setnilV(o);
  return o+1;
}
#else
#define api_call_base(L, nargs)	(L->top - (nargs))
#endif

LUA_API void lua_call(lua_State *L, int nargs, int nresults)
{
  lje_redirect_state(L);
  if (tvisfunc(L->base))
  {
    GCproto* p = funcproto(funcV(L->base));
    if (proto_chunkname(p))
    {
      LJE_DEBUG("Function being called from lua_pcall: %s", p ? proto_chunknamestr(p) : "unknown");
    }
  }

  api_check(L, L->status == LUA_OK || L->status == LUA_ERRERR);
  api_checknelems(L, nargs+1);
  lj_vm_call(L, api_call_base(L, nargs), nresults+1);
}

static void lje_dump_stack(lua_State* L)
{
  /* Really simple stack dump for debugging purposes. */
  /* It prints out a visual like this:
   * [function][number][number][string]
   * ----------------------------^ = L->top
   */

  int top = lua_gettop(L);
  int chars_printed = 0;
  /* Use lje_log_raw (no tag) so the alignment math below stays correct. */
  lje_log_raw("[LJE STACK DUMP] Stack (top=%d):", top);
  chars_printed += strlen("[LJE STACK DUMP] Stack (top=XX):");
  for (int i = 1; i <= top; i++)
  {
    int t = lua_type(L, i);
    const char* tname = lua_typename(L, t);
    lje_log_raw("[%s]", tname);
    chars_printed += (int)(strlen(tname) + 2); // +2 for the brackets
  }
  lje_log_raw("\n");
  for (int i = 0; i < chars_printed - 8; i++)
    lje_log_raw("-");
  lje_log_raw("^ = L->top\n");
}

/* LJE: lua_pcall is essentially the Lua entrypoint for the entire game.
 * We've co-opted it to perform a lot of orthogonal tasks related to LJE,
 * like initializing our functions, running preinit scripts, handling engine calls,
 * and reloading scripts as needed.
 */
LUA_API int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
{
  lje_redirect_state(L);

  if (tvisfunc(L->base) && LJEG()->waiting_for_init_call)
  {
    LJEG()->waiting_for_init_call = 0;
    LJEG()->using_error_reporter = 1;

    lje_clear_global_refs();
    LJEG()->main_state = L;
    LJEG()->using_error_reporter = 0;

    /* main_state is only now known; bind LJE_CLIENT_STATE before secure scripts run. */
    lje_path_install_state_globals(LJEG()->isolated_state);

    /* Closing the previous client state cleared its shadow registry, so re-seed. */
    lje_startup_shadow_stubs(LJEG()->isolated_state);
    lje_startup_secure_preinit(LJEG()->isolated_state);
    // Ensure settings are refetched as well
    lje_settings_clear_cache(LJEG()->isolated_state);
    for (int i = 0; i < LJEG()->loaded_script_count; i++)
    {
      LJEScript* script = LJEG()->script_load_order[i];
      LJE_INFO("Running script %s...", script->info->name);
      lje_startup_execute(LJEG()->isolated_state, script, NULL);
    }
  }

  /* LJE: Next, check if we're waiting for the startup call. */
  if (tvisfunc(L->base) && LJEG()->waiting_for_startup_call)
  {
    LJEG()->waiting_for_startup_call = 0;

    /* LJE: Create script watcher since now we're going to be running. */
    LJEG()->script_watcher = lje_watcher_create();
    for (int i = 0; i < LJEG()->loaded_script_count; i++)
    {
      LJEScript* script = LJEG()->script_load_order[i];
      lje_watcher_add_script(LJEG()->script_watcher, script);
    }

    lje_watcher_start(LJEG()->script_watcher);
    LJE_SUCCESS("Created script watcher for startup scripts.");

    LJE_INFO("Starting up Lua...");
  }

  /* LJE: Run all boot.lua entrypoints */
  if (tvisfunc(L->base) && LJEG()->waiting_for_menu_call)
  {
    LJEG()->waiting_for_menu_call = 0;
    LJE_INFO("Running boot entrypoint for all scripts...");
    // Boot scripts are the earliest thing that can call into a host state, so the
    // shadow registries need their stubs before the helpers land.
    lje_startup_shadow_stubs(LJEG()->isolated_state);
    // Load the pure-Lua helpers into the isolated state so boot scripts have them
    // available before they run.
    lje_startup_secure_helpers(LJEG()->isolated_state);

    // Check if any scripts have boot.lua, if so run them now in the isolated state
    LJEG()->in_boot_phase = 1;
    lje_iterate_scripts()
      if (script->boot_path != NULL)
      {
        LJE_INFO("Running boot script for script %s...", script->info->name);
        lje_startup_execute(LJEG()->isolated_state, script, script->boot_path);
      }
    lje_iterate_scripts_end()
    LJEG()->in_boot_phase = 0;
}

  /* LJE: Reload any scripts at this point, if needed. */
  if (LJEG()->script_watcher && LJEG()->main_state == L) /* only reload on new engine calls */
  {
    size_t scripts_needing_reload = lje_watcher_reload_count(LJEG()->script_watcher);
    if (scripts_needing_reload > 0)
    {
      LJE_INFO("Detected %zu scripts needing reload. Reloading now...", scripts_needing_reload);
      for (size_t i = 0; i < scripts_needing_reload; i++)
      {
        LJEScript* script = lje_watcher_pop_reload(LJEG()->script_watcher);
        lua_State* state = LJEG()->isolated_state;
        if (script)
        {

          lje_startup_reload(state, script);
        }
      }
    }
  }

  /* LJE: Post engine call hooks run *after* the real call below. We snapshot the
   * arguments onto the isolated state before the call consumes them, then replay a
   * copy of that snapshot to each post hook. */
  int run_post_hooks = 0;
  int post_snapshot_base = 0;
  int suppressed = 0;
  GCfunc* engine_func = NULL;
  LJEHostId call_host = LJE_HOST_CLIENT;

  /* LJE: Determine if this is an engine call. */
  /* Note: Not all engine calls start at the base of the stack.
   *
   * Some are called during Lua (e.g: in a C function), so we need to pivot around the **top** of the stack instead, using
   * api_call_base & the errfunc to determine where the function actually is.
   */
  /* Every CLuaInterface shares one AdvancedLuaErrorReporter, so the same check
     identifies an engine call in the menu universe as in the client one. */
  cTValue* errfunc_tv = errfunc ? stkindex2adr(L, errfunc) : NULL;
  if (lje_host_id_of(L, &call_host) && !LJEG()->using_error_reporter && errfunc &&
      tvisfunc(errfunc_tv) && tvisfunc(stkindex2adr(L, -nargs - 1)))
  {
    GCfunc* f = funcV(errfunc_tv);
    GCfunc* called_function = funcV(stkindex2adr(L, -nargs - 1));
    char is_function_null = f == LJ_GCVMASK || (uintptr_t)f == 0x0000400000000000;
    if (is_function_null)
    {
      LJE_WARN("errfunc is set but function is null. This is unexpected, but we'll try to continue anyway.");
      LJE_WARN("This often signals the stack is corrupted. Prepare for potential crash.");
    }

    if (!is_function_null && iscfunc(f))
    {
      /* Secure scripts can observe engine call hooks. We copy the stack to the isolated
       * state and call the hook there. Functions are passed as simple lightud pointers so
       * scripts can do comparisons without copying a full function object across states. */
      lua_State* I = LJEG()->isolated_state;

      /* Pre-call engine call hooks run first. While they do, they may call
       * lje.vm.suppress_engine_call() to drop the call entirely. Both flags are
       * saved and restored so a nested engine call can't clobber an outer one. */
      char prev_in_pre_hook = LJEG()->in_pre_engine_call_hook;
      char prev_suppressed = LJEG()->engine_call_suppressed;
      LJEG()->in_pre_engine_call_hook = 1;
      LJEG()->engine_call_suppressed = 0;
      for (size_t i = 0; i < LJEG()->loaded_script_count; i++) {
        LJEScript* script = LJEG()->script_load_order[i];
        for (size_t j = 0; j < script->extra->engine_call_hook_count; j++)
        {
          LJEEngineCallHook hook = script->extra->engine_call_hooks[j];
          if (hook.ref == LUA_NOREF)
            continue;
          if (hook.host != call_host)
            continue; /* registered for the other universe */
          if (hook.is_post)
          {
            run_post_hooks = 1; /* defer this one until after the real call */
            continue;
          }

          lua_rawgeti(I, LUA_REGISTRYINDEX, hook.ref);
          // Push the function that is being called as a pointer so we can do fast equality checks later
          lua_pushnumber(I, (lua_Number)((uintptr_t)called_function));
          lua_pushinteger(I, nargs);
          lua_pushinteger(I, nresults);
          for (int arg = 0; arg < nargs; arg++)
          {
            cTValue* arg_val = stkindex2adr(L, -nargs + arg);
            if (tvistab(arg_val) || tvisudata(arg_val))
              lua_pushlightuserdata(I, lje_proxy(arg_val, call_host));
            else
              lje_copy_to_isolated_state(L, I, -nargs + arg, 0);
          }

          LJEG()->using_error_reporter = 1;
          LJEG()->redirect_to_isolation = 0;
          if (lua_pcall(I, 3 + nargs, 0, 0) != LUA_OK)
          {
            LJEG()->using_error_reporter = 0;
            const char* error_msg = lua_tostring(I, -1);
            LJE_ERROR("Error in engine call hook for secure script %s: %s", script->info->name, error_msg);
            lua_pop(I, 1);
            lje_proxy_release_all();
            lj_gc_check(I);
            /* A hook that errored out doesn't get to suppress the call. */
            LJEG()->engine_call_suppressed = 0;
            continue;
          }

          LJEG()->using_error_reporter = 0;
          lje_proxy_release_all();
          lj_gc_check(I);

          if (LJEG()->engine_call_suppressed)
          {
            suppressed = 1;
            break; /* the remaining hooks are cancelled along with the call */
          }
        }

        if (suppressed)
          break;
      }

      LJEG()->in_pre_engine_call_hook = prev_in_pre_hook;
      LJEG()->engine_call_suppressed = prev_suppressed;

      /* LJE: Snapshot the args for post hooks before the real call consumes them. The
       * snapshot sits at the base of the isolated stack and we replay a copy to each
       * post hook afterwards; proxies stay alive until they have all run. */
      if (run_post_hooks && !suppressed)
      {
        engine_func = called_function;
        post_snapshot_base = lua_gettop(I) + 1;
        for (int arg = 0; arg < nargs; arg++)
        {
          cTValue* arg_val = stkindex2adr(L, -nargs + arg);
          if (tvistab(arg_val) || tvisudata(arg_val))
            lua_pushlightuserdata(I, lje_proxy(arg_val, call_host));
          else
            lje_copy_to_isolated_state(L, I, -nargs + arg, 0);
        }
      }
    }
  }

  /* A pre hook suppressed the call: pop the function and its arguments, then fake a
   * successful call that returned nothing, so the engine sees the shape it expects. */
  if (suppressed)
  {
    L->top -= nargs + 1;
    if (nresults > 0)
    {
      lj_state_checkstack(L, (MSize)nresults);
      for (int i = 0; i < nresults; i++)
        setnilV(L->top++);
    }
    return LUA_OK;
  }

  if (LJEG()->isolated_state == L && errfunc)
  {
    // Ensure it's not nil. This can sometimes happen, for reasons still unknown to me. Anyway, if its, just set to 0 so we handle it.
    cTValue* o = stkindex2adr(L, errfunc);
    if (o && !tvisfunc(o))
    {
      LJE_WARN("errfunc is set but not a function. This is unexpected, but we'll try to continue anyway.");
      lje_plat_debug_break();
    }
  }

  global_State *g = G(L);
  uint8_t oldh = hook_save(g);
  ptrdiff_t ef;
  int status;
  api_check(L, L->status == LUA_OK || L->status == LUA_ERRERR);
  api_checknelems(L, nargs+1);
  if (errfunc == 0) {
    ef = 0;
  } else {
    cTValue *o = stkindex2adr(L, errfunc);
    api_checkvalidindex(L, o);
    ef = savestack(L, o);
  }
  status = lj_vm_pcall(L, api_call_base(L, nargs), nresults+1, ef);
  if (status) hook_restore(g, oldh);
  if (status == LUA_ERRERR)
  {
    LJE_ERROR("ERRERR detected. state = %p", L);
    if (L == LJEG()->isolated_state)
    {
      LJE_ERROR("  + is isolated state!!!");
    }
  }

  /* Run all post engine call hooks now that the real call has completed. */
  if (run_post_hooks)
  {
    lua_State* I = LJEG()->isolated_state;
    for (size_t i = 0; i < LJEG()->loaded_script_count; i++) {
      LJEScript* script = LJEG()->script_load_order[i];
      for (size_t j = 0; j < script->extra->engine_call_hook_count; j++)
      {
        LJEEngineCallHook hook = script->extra->engine_call_hooks[j];
        if (hook.ref == LUA_NOREF)
          continue;
        if (!hook.is_post || hook.host != call_host)
          continue;

        lua_rawgeti(I, LUA_REGISTRYINDEX, hook.ref);
        lua_pushnumber(I, (lua_Number)((uintptr_t)engine_func));
        lua_pushinteger(I, nargs);
        lua_pushinteger(I, nresults);
        for (int arg = 0; arg < nargs; arg++)
          lua_pushvalue(I, post_snapshot_base + arg);

        LJEG()->using_error_reporter = 1;
        LJEG()->redirect_to_isolation = 0;
        if (lua_pcall(I, 3 + nargs, 0, 0) != LUA_OK)
        {
          LJEG()->using_error_reporter = 0;
          const char* error_msg = lua_tostring(I, -1);
          LJE_ERROR("Error in post engine call hook for secure script %s: %s", script->info->name, error_msg);
          lua_pop(I, 1);
          continue;
        }
        LJEG()->using_error_reporter = 0;
      }
    }

    /* Drop the snapshot and release the proxies now that every post hook has run. */
    lua_settop(I, post_snapshot_base - 1);
    lje_proxy_release_all();
    lj_gc_check(I);
  }

  return status;
}

static TValue *cpcall(lua_State *L, lua_CFunction func, void *ud)
{
  GCfunc *fn = lj_func_newC(L, 0, getcurrenv(L));
  TValue *top = L->top;
  fn->c.f = func;
  setfuncV(L, top++, fn);
  if (LJ_FR2) setnilV(top++);
  setlightudV(top++, checklightudptr(L, ud));
  cframe_nres(L->cframe) = 1+0;  /* Zero results. */
  L->top = top;
  return top-1;  /* Now call the newly allocated C function. */
}

LUA_API int lua_cpcall(lua_State *L, lua_CFunction func, void *ud)
{
  lje_redirect_state(L);
  global_State *g = G(L);
  uint8_t oldh = hook_save(g);
  int status;
  api_check(L, L->status == LUA_OK || L->status == LUA_ERRERR);
  status = lj_vm_cpcall(L, func, ud, cpcall);
  if (status) hook_restore(g, oldh);
  return status;
}

LUALIB_API int luaL_callmeta(lua_State *L, int idx, const char *field)
{
  lje_redirect_state(L);
  if (luaL_getmetafield(L, idx, field)) {
    TValue *top = L->top--;
    if (LJ_FR2) setnilV(top++);
    copyTV(L, top++, index2adr(L, idx));
    L->top = top;
    lj_vm_call(L, top-1, 1+1);
    return 1;
  }
  return 0;
}

/* -- Coroutine yield and resume ------------------------------------------ */

LUA_API int lua_isyieldable(lua_State *L)
{
  return cframe_canyield(L->cframe);
}

LUA_API int lua_yield(lua_State *L, int nresults)
{
  lje_redirect_state(L);
  void *cf = L->cframe;
  global_State *g = G(L);
  if (cframe_canyield(cf)) {
    cf = cframe_raw(cf);
    if (!hook_active(g)) {  /* Regular yield: move results down if needed. */
      cTValue *f = L->top - nresults;
      if (f > L->base) {
	TValue *t = L->base;
	while (--nresults >= 0) copyTV(L, t++, f++);
	L->top = t;
      }
      L->cframe = NULL;
      L->status = LUA_YIELD;
      return -1;
    } else {  /* Yield from hook: add a pseudo-frame. */
      TValue *top = L->top;
      hook_leave(g);
      (top++)->u64 = cframe_multres(cf);
      setcont(top, lj_cont_hook);
      if (LJ_FR2) top++;
      setframe_pc(top, cframe_pc(cf)-1);
      if (LJ_FR2) top++;
      setframe_gc(top, obj2gco(L), LJ_TTHREAD);
      setframe_ftsz(top, ((char *)(top+1)-(char *)L->base)+FRAME_CONT);
      L->top = L->base = top+1;
#if LJ_TARGET_X64
      lj_err_throw(L, LUA_YIELD);
#else
      L->cframe = NULL;
      L->status = LUA_YIELD;
      lj_vm_unwind_c(cf, LUA_YIELD);
#endif
    }
  }
  lj_err_msg(L, LJ_ERR_CYIELD);
  return 0;  /* unreachable */
}

LUA_API int lua_resume(lua_State *L, int nargs)
{
  if (L->cframe == NULL && L->status <= LUA_YIELD)
    return lj_vm_resume(L,
      L->status == LUA_OK ? api_call_base(L, nargs) : L->top - nargs,
      0, 0);
  L->top = L->base;
  setstrV(L, L->top, lj_err_str(L, LJ_ERR_COSUSP));
  incr_top(L);
  return LUA_ERRRUN;
}

/* -- GC and memory management -------------------------------------------- */

LUA_API int lua_gc(lua_State *L, int what, int data)
{
  lje_redirect_state(L);
  global_State *g = G(L);
  int res = 0;
  switch (what) {
  case LUA_GCSTOP:
    g->gc.threshold = LJ_MAX_MEM;
    break;
  case LUA_GCRESTART:
    g->gc.threshold = data == -1 ? (g->gc.total/100)*g->gc.pause : g->gc.total;
    break;
  case LUA_GCCOLLECT:
    lj_gc_fullgc(L);
    break;
  case LUA_GCCOUNT:
    res = (int)(g->gc.total >> 10);
    break;
  case LUA_GCCOUNTB:
    res = (int)(g->gc.total & 0x3ff);
    break;
  case LUA_GCSTEP: {
    GCSize a = (GCSize)data << 10;
    g->gc.threshold = (a <= g->gc.total) ? (g->gc.total - a) : 0;
    while (g->gc.total >= g->gc.threshold)
      if (lj_gc_step(L) > 0) {
	res = 1;
	break;
      }
    break;
  }
  case LUA_GCSETPAUSE:
    res = (int)(g->gc.pause);
    g->gc.pause = (MSize)data;
    break;
  case LUA_GCSETSTEPMUL:
    res = (int)(g->gc.stepmul);
    g->gc.stepmul = (MSize)data;
    break;
  case LUA_GCISRUNNING:
    res = (g->gc.threshold != LJ_MAX_MEM);
    break;
  default:
    res = -1;  /* Invalid option. */
  }
  return res;
}

LUA_API lua_Alloc lua_getallocf(lua_State *L, void **ud)
{
  global_State *g = G(L);
  if (ud) *ud = g->allocd;
  return g->allocf;
}

LUA_API void lua_setallocf(lua_State *L, lua_Alloc f, void *ud)
{
  lje_redirect_state(L);
  global_State *g = G(L);
  g->allocd = ud;
  g->allocf = f;
}

// Anything to do with new states is hooked - not overwritten.
// This is because GMod has a lot of proprietary changes that we can't really replicate easily

typedef void (*lua_close_t)(lua_State* L);

static LJEDetourHook lua_close_hook;

static void lua_close_detour(lua_State* L)
{
  int host_closed = lje_host_id_of(L, NULL);

  if (L == LJEG()->main_state)
  {
    LJE_INFO("Detected main state being closed. Cleaning up LJE resources...");
    lje_iterate_scripts()
      if (script->extra->cleanup_ref_id != LUA_NOREF)
      {
        lua_State* I = LJEG()->isolated_state;
        lua_rawgeti(I, LUA_REGISTRYINDEX, script->extra->cleanup_ref_id);
        if (lua_isfunction(I, -1))
        {
          LJE_INFO("Running cleanup for script %s...", script->info->name);
          if (lua_pcall(I, 0, 0, 0) != LUA_OK)
          {
            const char* error_msg = lua_tostring(I, -1);
            LJE_ERROR("Error in cleanup for script %s: %s", script->info->name, error_msg);
            lua_pop(I, 1); // pop error message
          }
        } else
        {
          lua_pop(I, 1); // pop non-function
          LJE_INFO("No cleanup function found for script %s.", script->info->name);
        }
      } else {
        LJE_INFO("No cleanup needed for script %s.", script->info->name);
      }
    lje_iterate_scripts_end()

    // Fully run a GC pass on the isolated state
    lj_gc_fullgc(LJEG()->isolated_state);
    LJEG()->main_state = NULL;
    lje_clear_global_refs();
    // Kill off old registry.
    if (lje_host_view(LJE_HOST_CLIENT)->shadow_registry)
      lj_tab_clear(lje_host_view(LJE_HOST_CLIENT)->shadow_registry);
  }
  else if (L == LJEG()->menu_state)
  {
    LJE_INFO("Detected menu state being closed.");
    LJEG()->menu_state = NULL;
    if (lje_host_view(LJE_HOST_MENU)->shadow_registry)
      lj_tab_clear(lje_host_view(LJE_HOST_MENU)->shadow_registry);
  }

  /* lje.state holds raw pointers, so drop the one that just died. */
  if (host_closed && LJEG()->isolated_state)
    lje_path_install_state_globals(LJEG()->isolated_state);

  /* Lift the hook, run the real lua_close, then re-arm it for the next state. */
  if (lua_close_hook.target)
  {
    lje_detour_suspend(&lua_close_hook);
    ((lua_close_t)lua_close_hook.target)(L);
    lje_detour_resume(&lua_close_hook);
  }
}

#ifdef LJ_TARGET_WINDOWS
#include <windows.h>
#include <stdio.h>

/* Bootstrap — extracted from DllMain so the same initialisation sequence can
 * later be called from a __attribute__((constructor)) shim on POSIX. */
static void lje_bootstrap(void* self_handle)
{
  lje_plat_init(self_handle);
  lje_plat_console_init("LJE Console");

  lje_Module* mod = lje_module_find(LJE_LUA_MODULE);
    if (mod) {
      LJECommandLineOptions* options = lje_get_command_line_options();
      if (options->enable_debug_prints)
        lje_log_set_min_level(LJE_LOG_DEBUG);

      printf("\n");
      lje_log_banner();
      printf("\n\n");
      LJE_SUCCESS("LJE loaded successfully!");
      LJE_DEBUG("lua_shared.dll found at %p", mod->base);

      LJE_INFO("Initializing crash handler...");
      lje_crash_handler_init();
      LJE_SUCCESS("Crash handler initialized!");

      // Try migrating
      if (!lje_migrate_legacy_dirs())
      {
        LJE_ERROR("Failed to migrate legacy directories! If you had scripts or binary modules in the old folders, please move them to the new ones manually.");
      }

// This is *very* annoying, but GMod's luaL_traceback seems to have inlined lj_debug_frame,
// so we have to detour that as well to ensure our debug_frame is used.
lje_detour_export(mod, luaL_traceback, luaL_traceback);
// Necessary because debug.getupvalue simply just calls into this
lje_detour_export(mod, lua_getupvalue, lua_getupvalue);
lje_detour_export(mod, lua_pushstring, lua_pushstring);
lje_detour_export(mod, lua_pcall, lua_pcall);
lje_detour_export(mod, luaL_loadbufferx, luaL_loadbufferx);

// Stack top / manipulation
lje_detour_export(mod, lua_gettop, lua_gettop);
lje_detour_export(mod, lua_settop, lua_settop);
lje_detour_export(mod, lua_pushvalue, lua_pushvalue);
lje_detour_export(mod, lua_checkstack, lua_checkstack);

// Push (C -> stack)
lje_detour_export(mod, lua_pushnil, lua_pushnil);
lje_detour_export(mod, lua_pushnumber, lua_pushnumber);
lje_detour_export(mod, lua_pushinteger, lua_pushinteger);
lje_detour_export(mod, lua_pushboolean, lua_pushboolean);
lje_detour_export(mod, lua_pushlstring, lua_pushlstring);
lje_detour_export(mod, lua_pushcclosure, lua_pushcclosure);
lje_detour_export(mod, lua_pushlightuserdata, lua_pushlightuserdata);

// To (stack -> C)
lje_detour_export(mod, lua_tonumber, lua_tonumber);
lje_detour_export(mod, lua_tointeger, lua_tointeger);
lje_detour_export(mod, lua_toboolean, lua_toboolean);
lje_detour_export(mod, lua_tolstring, lua_tolstring);
lje_detour_export(mod, lua_touserdata, lua_touserdata);
lje_detour_export(mod, lua_tocfunction, lua_tocfunction);

// Type checks
lje_detour_export(mod, lua_type, lua_type);
lje_detour_export(mod, lua_isnumber, lua_isnumber);
lje_detour_export(mod, lua_isstring, lua_isstring);
lje_detour_export(mod, lua_iscfunction, lua_iscfunction);
lje_detour_export(mod, lua_isuserdata, lua_isuserdata);

// Table get
lje_detour_export(mod, lua_gettable, lua_gettable);
lje_detour_export(mod, lua_getfield, lua_getfield);
lje_detour_export(mod, lua_rawget, lua_rawget);
lje_detour_export(mod, lua_rawgeti, lua_rawgeti);

// Table set
lje_detour_export(mod, lua_settable, lua_settable);
lje_detour_export(mod, lua_setfield, lua_setfield);
lje_detour_export(mod, lua_rawset, lua_rawset);
lje_detour_export(mod, lua_rawseti, lua_rawseti);

// Construction
lje_detour_export(mod, lua_createtable, lua_createtable);
lje_detour_export(mod, lua_newuserdata, lua_newuserdata);

// Call

      // Stack reordering
      lje_detour_export(mod, lua_remove, lua_remove);
      lje_detour_export(mod, lua_insert, lua_insert);
      lje_detour_export(mod, lua_replace, lua_replace);
      lje_detour_export(mod, lua_copy, lua_copy);

      // Formatted push (likely culprits for variadic issues)
      lje_detour_export(mod, lua_pushfstring, lua_pushfstring);
      lje_detour_export(mod, lua_pushvfstring, lua_pushvfstring);

      // Length / comparisons
      lje_detour_export(mod, lua_objlen, lua_objlen);
      lje_detour_export(mod, lua_equal, lua_equal);
      lje_detour_export(mod, lua_rawequal, lua_rawequal);
      lje_detour_export(mod, lua_lessthan, lua_lessthan);

      // Metatable access
      lje_detour_export(mod, lua_getmetatable, lua_getmetatable);
      lje_detour_export(mod, lua_setmetatable, lua_setmetatable);

      // Iteration / concat / errors
      lje_detour_export(mod, lua_next, lua_next);
      lje_detour_export(mod, lua_concat, lua_concat);
      lje_detour_export(mod, lua_error, lua_error);

      // === Tier 3 ===

      // Identity / debug-ish
      lje_detour_export(mod, lua_topointer, lua_topointer);

      // Coroutines / threads (translate state pointers if you have a mapping)

      // Misc state
      lje_detour_export(mod, lua_status, lua_status);
      lje_detour_export(mod, lua_atpanic, lua_atpanic);

      lje_detour_export(mod, luaL_checklstring, luaL_checklstring);
      lje_detour_export(mod, luaL_optlstring, luaL_optlstring);
      lje_detour_export(mod, luaL_checknumber, luaL_checknumber);
      lje_detour_export(mod, luaL_optnumber, luaL_optnumber);
      lje_detour_export(mod, luaL_checkinteger, luaL_checkinteger);
      lje_detour_export(mod, luaL_optinteger, luaL_optinteger);
      lje_detour_export(mod, luaL_checktype, luaL_checktype);
      lje_detour_export(mod, luaL_checkany, luaL_checkany);
      lje_detour_export(mod, luaL_checkudata, luaL_checkudata);

      // Errors
      lje_detour_export(mod, luaL_error, luaL_error);
      lje_detour_export(mod, luaL_argerror, luaL_argerror);
      lje_detour_export(mod, luaL_typerror, luaL_typerror);
      lje_detour_export(mod, luaL_where, luaL_where);

      // Metatables

      // Refs (the registry-ref system from before)
      lje_detour_export(mod, luaL_ref, luaL_ref);
      lje_detour_export(mod, luaL_unref, luaL_unref);

      // Library registration (engine registers things at startup)
      lje_detour_export(mod, luaL_register, luaL_register);
      lje_detour_export(mod, luaL_openlib, luaL_openlib);
      lje_detour_export(mod, luaL_setfuncs, luaL_setfuncs);

      // Metafield/callmeta (occasional but real)
      lje_detour_export(mod, luaL_getmetafield, luaL_getmetafield);
      lje_detour_export(mod, luaL_callmeta, luaL_callmeta);

      // === Tier 2 luaL ===

      lje_detour_export(mod, luaL_checkoption, luaL_checkoption);
      lje_detour_export(mod, luaL_checkstack, luaL_checkstack);  // note: different from lua_checkstack
      lje_detour_export(mod, luaL_gsub, luaL_gsub);
      lje_detour_export(mod, luaL_findtable, luaL_findtable);
      lje_detour_export(mod, luaL_testudata, luaL_testudata);
      lje_detour_export(mod, luaL_setmetatable, luaL_setmetatable);
      lje_detour_export(mod, luaL_pushmodule, luaL_pushmodule);

      void* original_close = lje_module_get_func(mod, "lua_close");
      if (original_close)
      {
        int attached = lje_detour_hook(&lua_close_hook, original_close, (void*)lua_close_detour);
        if (!attached)
          LJE_ERROR("Failed to detour lua_close! This may cause resource leaks when the game closes.");
        else
          LJE_DEBUG("Detoured lua_close successfully!");
      }

      if (options->disable_binary_modules)
      {
        LJE_INFO("Binary module loading is disabled via command line option, skipping...");
        LJEG()->loaded_binary_module_count = 0;
        LJEG()->loaded_binary_modules = NULL;
      } else
      {
        LJE_INFO("Loading binary modules...");
        lje_binary_module_ensure_folder_exists();
        lje_binary_module_load_all(&LJEG()->loaded_binary_module_count, &LJEG()->loaded_binary_modules);
        if (LJEG()->loaded_binary_module_count > 0)
        {
          LJE_INFO("Loaded %d binary modules:", LJEG()->loaded_binary_module_count);
          for (int i = 0; i < LJEG()->loaded_binary_module_count; i++)
          {
            LJEBinaryModule module = LJEG()->loaded_binary_modules[i];
            LJE_INFO("- %s", module.name);
          }
        } else {
          LJE_INFO("No binary modules loaded.");
        }
      }

      if (!lje_script_folder_exists())
      {
        // Tell them we're creating one for them
        char path[LJE_PATH_MAX];
        lje_script_resolve_base(path, LJE_PATH_MAX);
        LJE_INFO("%s folder not found, creating it now...", LJE_SCRIPT_FOLDER);
        LJE_INFO("Creating at path: %s", path);
        if (!lje_script_folder_create())
        {
          LJE_ERROR("Failed to create %s folder! Please create it manually.", LJE_SCRIPT_FOLDER);
        } else {
          LJE_SUCCESS("Successfully created %s folder!", LJE_SCRIPT_FOLDER);
        }
      }

      if (!options->disable_scripts)
      {
        LJEG()->loaded_scripts = lje_script_load_all_scripts(&LJEG()->loaded_script_count);
        LJE_SUCCESS("Loaded %llu scripts!", LJEG()->loaded_script_count);
        for (size_t i = 0; i < LJEG()->loaded_script_count; i++)
        {
          LJE_INFO("- %s", LJEG()->loaded_scripts[i].name);
          LJE_INFO("  Name: %s", LJEG()->loaded_scripts[i].info->name);
          LJE_INFO("  Author: %s", LJEG()->loaded_scripts[i].info->author);
          LJE_INFO("  Version: %s", LJEG()->loaded_scripts[i].info->version);
          for (size_t j = 0; j < LJEG()->loaded_scripts[i].info->dependency_count; j++)
          {
            LJE_INFO("  Dependency: %s", LJEG()->loaded_scripts[i].info->dependencies[j].name);
          }

          if (LJEG()->loaded_scripts[i].info->binary_dependencies)
          {
            for (size_t j = 0; j < LJEG()->loaded_scripts[i].info->binary_dependency_count; j++)
            {
              LJE_INFO("  Binary Dependency: %s", LJEG()->loaded_scripts[i].info->binary_dependencies[j].name);
            }
          }
        }

        LJE_DEBUG("Performing dependency resolution...");
        LJEG()->script_load_order = lje_script_compute_load_order(
          LJEG()->loaded_script_count,
          LJEG()->loaded_scripts
        );

        for (size_t i = 0; i < LJEG()->loaded_script_count; i++)
        {
          LJEScript* script = LJEG()->script_load_order[i];
          LJE_DEBUG("- %d. %s", i + 1, script->name);
        }
      } else
      {
        LJE_INFO("Script loading is disabled via command line option, skipping...");
        LJEG()->loaded_script_count = 0;
        LJEG()->loaded_scripts = NULL;
      }

      lje_proxy_arena_init();
      LJEG()->isolated_state = lje_create_isolated_state();
      if (LJEG()->isolated_state)
      {
        LJE_DEBUG("Created isolated Lua state for secure scripts.");
      }

      for (int i = 0; i < LJEG()->loaded_binary_module_count; i++)
      {
        LJEBinaryModule* mod = &LJEG()->loaded_binary_modules[i];
        LJE_INFO("Loading binary module %s into isolated state...", mod->name);
        lje_binary_module_run_preinit(mod, LJEG()->isolated_state);
      }
    } else {
      LJE_ERROR(LJE_LUA_MODULE " not found!");
    }
  }

  /* ---- DllMain --------------------------------------------------------- */
  /* On POSIX this whole function is replaced by __attribute__((constructor)). */

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH)
  {
    DisableThreadLibraryCalls(hModule);
    lje_bootstrap(hModule);
    return TRUE;
  }

  return TRUE;
}
#endif
