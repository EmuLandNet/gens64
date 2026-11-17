/*
** $Id: lapi.h,v 2.2.1.1 2007/12/27 13:02:25 roberto Exp $
** Auxiliary functions from Lua API
** See Copyright Notice in lua.h
*/

#ifndef lapi_h
#define lapi_h


#include "lobject.h"
#include "lstate.h"


LUAI_FUNC void luaA_pushobject (lua_State *L, const TValue *o);

/* gens64: MSVC emitted an external definition for this _inline function; GCC/clang (gnu89/MS __inline)
   do not, so define it out-of-line in lapi.c instead. */
#ifdef GENS64_LAPI_GETTOP_DEF
LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - L->base);
}
#else
LUA_API int (lua_gettop) (lua_State *L);
#endif

#endif
