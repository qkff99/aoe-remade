/*
** Userdata handling.
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_udata.h"

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = lj_mem_newt(L, sizeof(GCudata) + sz, GCudata);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->unused2 = 0;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->scanprev, obj2gco(mainthread(g)));
  setgcref(ud->env, obj2gco(env));
  /* Chain to userdata list (after main thread). */
  setgcrefr(ud->nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, obj2gco(ud));
  if (g->gc.udscanmode) lj_gc_udata_link(g, obj2gco(ud));
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  lj_mem_free(g, ud, sizeudata(ud));
}

