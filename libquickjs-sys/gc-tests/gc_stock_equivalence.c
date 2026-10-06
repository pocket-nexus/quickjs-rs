/* A host that never calls the minor-collector API must see the stock
   collector: the same automatic and explicit collections, the same
   objects freed in the same order, the same memory accounting.

   The program uses only the stock API and prints a trace (memory usage
   after every step, finalizer order). run.sh compares it with the trace of
   the same program built against a stock QuickJS. The runtime struct itself
   is left out of malloc_size so that a larger JSRuntime does not move the
   size-based trigger, and memory_used_size (which counts it) is printed
   relative to the first reading. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "quickjs.h"

static JSClassID fin_class;
static int skip_first_alloc = 1;

static void *t_malloc(JSMallocState *s, size_t size)
{
    void *p = malloc(size);
    if (!p)
        return NULL;
    if (skip_first_alloc) {
        skip_first_alloc = 0; /* the JSRuntime */
        return p;
    }
    s->malloc_count++;
    s->malloc_size += malloc_usable_size(p);
    return p;
}

static void t_free(JSMallocState *s, void *p)
{
    if (!p)
        return;
    s->malloc_count--;
    s->malloc_size -= malloc_usable_size(p);
    free(p);
}

static void *t_realloc(JSMallocState *s, void *p, size_t size)
{
    size_t old = p ? malloc_usable_size(p) : 0;
    if (size == 0) {
        t_free(s, p);
        return NULL;
    }
    if (!p)
        return t_malloc(s, size);
    void *q = realloc(p, size);
    if (!q)
        return NULL;
    s->malloc_size += malloc_usable_size(q) - old;
    return q;
}

static size_t t_usable(const void *p) { return malloc_usable_size((void *)p); }

static void fin_finalizer(JSRuntime *rt, JSValue val)
{
    printf("fin %d\n", (int)(intptr_t)JS_GetOpaque(val, fin_class));
}

static JSValue js_mkfin(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    JSValue o = JS_NewObjectClass(ctx, fin_class);
    JS_SetOpaque(o, (void *)(intptr_t)id);
    return o;
}

static const char *SRC =
    "var keep = []; var wm = new WeakMap(); var sum = 0;\n"
    "function mk(n){ var a={n:n, h:mkfin(n)}; var b={a:a}; a.b=b; a.h.back=a;\n"
    "  a.f=function(){return a.n+b.a.n;}; return a; }\n"
    "async function af(x){ await null; return x+1; }\n"
    "function* g(){ var o={}; o.self=o; yield o; }\n"
    "globalThis.step = function(i){\n"
    "  for (var k=0;k<40;k++){ var c=mk(i*1000+k); sum=(sum+c.f())|0;\n"
    "    if(k%9==0) keep.push(c); }\n"
    "  if (keep.length > 120) { var old = keep.splice(0, 60);\n"
    "    old[0].back = keep[0]; keep[0].fwd = old; }\n"
    "  if (i%7==0) { var o={}; wm.set(o, mk(-i)); wm.set(keep[0], {x:keep[1]}); }\n"
    "  if (i%11==0) { var it=g(); it.next(); af(i).then(function(v){ sum=(sum+v)|0; }); }\n"
    "  if (i%13==0) { var m=new Map(); var s=new Set(); m.set(s,m); s.add(m);\n"
    "    keep[0].m = m; }\n"
    "  if (i%17==0) { keep.length = Math.min(keep.length, 10); }\n"
    "  return sum;\n"
    "};\n";

static long long boot_mem;

static void dump(JSRuntime *rt, const char *what, int i)
{
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(rt, &u);
    /* memory_used_size includes sizeof(JSRuntime): print it relative to
       the first dump */
    if (!boot_mem)
        boot_mem = u.memory_used_size;
    printf("%s %d malloc=%lld/%lld mem=%lld/%lld obj=%lld/%lld prop=%lld/%lld "
           "shape=%lld/%lld fn=%lld/%lld arr=%lld str=%lld atom=%lld\n",
           what, i, (long long)u.malloc_count, (long long)u.malloc_size,
           (long long)u.memory_used_count, (long long)u.memory_used_size - boot_mem,
           (long long)u.obj_count, (long long)u.obj_size,
           (long long)u.prop_count, (long long)u.prop_size,
           (long long)u.shape_count, (long long)u.shape_size,
           (long long)u.js_func_count, (long long)u.js_func_size,
           (long long)u.array_count, (long long)u.str_count,
           (long long)u.atom_count);
}

int main(void)
{
    static const JSMallocFunctions mf = { t_malloc, t_free, t_realloc, t_usable };
    static const JSClassDef def = { "Fin", .finalizer = fin_finalizer };
    JSRuntime *rt = JS_NewRuntime2(&mf, NULL);
    JSContext *ctx = JS_NewContext(rt);
    JS_NewClassID(&fin_class);
    JS_NewClass(rt, fin_class, &def);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "mkfin", JS_NewCFunction(ctx, js_mkfin, "mkfin", 1));
    JSValue r = JS_Eval(ctx, SRC, strlen(SRC), "<t>", 0);
    if (JS_IsException(r))
        return 1;
    JS_FreeValue(ctx, r);
    JSValue step = JS_GetPropertyStr(ctx, global, "step");
    dump(rt, "boot", 0);

    for (int i = 0; i < 300; i++) {
        JSValue a = JS_NewInt32(ctx, i);
        JSValue v = JS_Call(ctx, step, global, 1, &a);
        int32_t sum = 0;
        if (JS_IsException(v))
            return 1;
        JS_ToInt32(ctx, &sum, v);
        JS_FreeValue(ctx, v);
        JSContext *c1;
        while (JS_ExecutePendingJob(rt, &c1) > 0) {}
        printf("step %d sum=%d\n", i, sum);
        if (i % 25 == 24) {
            JS_RunGC(rt);
            dump(rt, "gc", i);
        } else {
            dump(rt, "step", i);
        }
    }
    JS_FreeValue(ctx, step);
    JS_FreeValue(ctx, global);
    JS_RunGC(rt);
    dump(rt, "end", 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("freed\n");
    return 0;
}
