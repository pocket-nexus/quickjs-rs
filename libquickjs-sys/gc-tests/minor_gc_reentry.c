/* Reentrancy tests for the minor collector (JS_SetGCYoungLimit,
   JS_RunGCMinor, JS_FreePendingObjects, JS_PromoteGCObjects).

   Each scenario lets host code allocate, collect or run JS while a
   collection, its hook or a pending release is in progress. Allocations
   made there must only be counted: no collection may start inside another
   one. Build with run.sh (ASan + UBSan, QuickJS asserts on).

   usage: minor_gc_reentry <scenario>   (no argument: list scenarios) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "quickjs.h"

static JSContext *g_ctx;
static int g_hook_depth, g_hook_max_depth, g_hook_begin, g_hook_end;
static int g_failures;
static JSClassID g_class_id;
static int g_finalized;
/* finalizers allocate while set; 2: they also force the size-based
   trigger for their own allocations */
static int g_finalizer_allocs = 0;
static int g_in_minor, g_in_pending; /* the test is inside that call */
static int g_fin_in_minor, g_fin_in_pending;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,      \
                    __LINE__, #cond);                                   \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* JS that builds young cycles of mixed shapes on every call. */
static const char *SRC =
    "var keep = []; var wm = new WeakMap(); var sum = 0;\n"
    "function mk(n){ var a={n:n}; var b={a:a}; a.b=b;\n"
    "  a.f=function(){return a.n+b.a.n;}; return a; }\n"
    "globalThis.step = function(i){\n"
    "  for (var k=0;k<50;k++){ var c=mk(k+i); sum=(sum+c.f())|0;\n"
    "    if(k%9==0) keep.push(c); }\n"
    "  if (keep.length > 200) { var old = keep.splice(0, 100);\n"
    "    old[0].back = keep[0]; keep[0].fwd = old; }\n"
    "  if (i%7==0) { var o={}; wm.set(o, mk(i)); wm.set(keep[0], {x:keep[1]}); }\n"
    "  if (i%13==0) { var m=new Map(); var s=new Set(); m.set(s,m); s.add(m);\n"
    "    keep[0].m = m; }\n"
    "  if (i%19==0) { keep.length = Math.min(keep.length, 20); }\n"
    "  return sum;\n"
    "};\n";

static void hook_enter(int end)
{
    g_hook_depth++;
    if (g_hook_depth > g_hook_max_depth)
        g_hook_max_depth = g_hook_depth;
    if (end)
        g_hook_end++;
    else
        g_hook_begin++;
}

static void hook_leave(void) { g_hook_depth--; }

static void alloc_one(void)
{
    JSValue o = JS_NewObject(g_ctx);
    CHECK(!JS_IsException(o));
    JS_FreeValue(g_ctx, o);
}

/* the reproducer from the review: one allocation in the begin hook */
static void hook_alloc_begin(JSRuntime *rt, int end)
{
    hook_enter(end);
    if (!end)
        alloc_one();
    hook_leave();
}

static void hook_alloc_end(JSRuntime *rt, int end)
{
    hook_enter(end);
    if (end) {
        /* the first object stays young while the second allocation
           sees the limit reached */
        JSValue o = JS_NewObject(g_ctx);
        alloc_one();
        JS_FreeValue(g_ctx, o);
    }
    hook_leave();
}

/* runs JS that allocates far more objects than the young limit */
static void hook_eval(JSRuntime *rt, int end)
{
    static const char code[] =
        "(function(){ var r=[]; for (var i=0;i<64;i++){ var o={i:i}; o.s=o;\n"
        "  r.push(o); } return r.length; })()";
    hook_enter(end);
    JSValue v = JS_Eval(g_ctx, code, sizeof(code) - 1, "<hook>", 0);
    CHECK(!JS_IsException(v));
    JS_FreeValue(g_ctx, v);
    hook_leave();
}

/* explicit collections from inside the hook */
static void hook_collect(JSRuntime *rt, int end)
{
    hook_enter(end);
    alloc_one();
    switch ((g_hook_begin + g_hook_end) % 4) {
    case 0: JS_RunGCMinor(rt); break;
    case 1: JS_FreePendingObjects(rt, 3); break;
    case 2: JS_PromoteGCObjects(rt); break;
    case 3: if (end) JS_RunGC(rt); break;
    }
    alloc_one();
    hook_leave();
}

/* A host class whose finalizer allocates. QuickJS (stock included) cannot
   let a finalizer revive a shape that the same collection is freeing, so
   the scenario keeps an empty object alive: the shape of the objects the
   finalizer creates is never garbage. */
static void fin_finalizer(JSRuntime *rt, JSValue val)
{
    uint32_t young0, young1, pending;
    JS_GetGCCounts(rt, &young0, &pending);
    g_finalized++;
    g_fin_in_minor += g_in_minor;
    g_fin_in_pending += g_in_pending;
    if (g_finalizer_allocs == 2)
        JS_SetGCThreshold(rt, 0);
    if (g_finalizer_allocs) {
        alloc_one();
        alloc_one();
    }
    if (g_finalizer_allocs == 2)
        JS_SetGCThreshold(rt, SIZE_MAX / 2);
    /* a collection started here would have made the young objects old */
    JS_GetGCCounts(rt, &young1, &pending);
    CHECK(young1 >= young0);
}

static void define_class(JSRuntime *rt)
{
    static const JSClassDef def = { "Fin", .finalizer = fin_finalizer };
    JS_NewClassID(&g_class_id);
    CHECK(JS_NewClass(rt, g_class_id, &def) == 0);
}

/* a cycle through a host object: freed by a collection, not by refcount */
static void make_fin_cycle(JSValue holder, int i)
{
    JSValue f = JS_NewObjectClass(g_ctx, g_class_id);
    JSValue o = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, o, "f", JS_DupValue(g_ctx, f));
    JS_SetPropertyStr(g_ctx, f, "o", JS_DupValue(g_ctx, o));
    if (!JS_IsUndefined(holder) && i % 3 == 0) {
        /* held by an old object, then dropped: released through the
           pending list */
        JS_SetPropertyUint32(g_ctx, holder, i % 16, JS_DupValue(g_ctx, o));
    }
    JS_FreeValue(g_ctx, f);
    JS_FreeValue(g_ctx, o);
}

/* host objects under an old array that only a young cycle references: a
   minor collection frees the cycle and queues the array, and
   JS_FreePendingObjects() then runs their finalizers */
static void make_pending_tree(JSRuntime *rt)
{
    JSValue tree = JS_NewArray(g_ctx);
    for (int k = 0; k < 4; k++)
        JS_SetPropertyUint32(g_ctx, tree, k, JS_NewObjectClass(g_ctx, g_class_id));
    JS_PromoteGCObjects(rt);
    JSValue a = JS_NewObject(g_ctx);
    JSValue b = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, a, "b", JS_DupValue(g_ctx, b));
    JS_SetPropertyStr(g_ctx, b, "a", JS_DupValue(g_ctx, a));
    JS_SetPropertyStr(g_ctx, a, "tree", tree);
    JS_FreeValue(g_ctx, a);
    JS_FreeValue(g_ctx, b);
}

static void run_minor(JSRuntime *rt)
{
    g_in_minor = 1;
    JS_RunGCMinor(rt);
    g_in_minor = 0;
}

typedef struct {
    const char *name;
    void (*hook)(JSRuntime *rt, int end);
    uint32_t young_limit;
    int fin; /* finalizer-allocation workload (g_finalizer_allocs) */
} Scenario;

static const Scenario scenarios[] = {
    { "hook-alloc-begin", hook_alloc_begin, 1, 0 },
    { "hook-alloc-end", hook_alloc_end, 1, 0 },
    { "hook-eval", hook_eval, 5, 0 },
    { "hook-eval-limit-1", hook_eval, 1, 0 },
    { "hook-collect", hook_collect, 2, 0 },
    { "finalizer-alloc", NULL, 1, 1 },
    { "finalizer-alloc-hook", hook_alloc_begin, 3, 1 },
    /* explicit minor collections and pending releases only */
    { "finalizer-alloc-explicit", NULL, 0, 2 },
};

static int run(const Scenario *sc)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSMemoryUsage u;
    int32_t sum = 0;
    int iters = 60;

    g_ctx = ctx;
    define_class(rt);
    JSValue r = JS_Eval(ctx, SRC, strlen(SRC), "<t>", 0);
    CHECK(!JS_IsException(r));
    JS_FreeValue(ctx, r);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue step = JS_GetPropertyStr(ctx, global, "step");
    JSValue holder = JS_NewArray(ctx);
    JSValue anchor = JS_NewObject(ctx);

    JS_PromoteGCObjects(rt);
    JS_SetGCYoungLimit(rt, sc->young_limit, sc->hook);
    /* the size-based automatic collection fires on every allocation too
       (it must not start inside a minor one either) */
    if (sc->fin)
        JS_SetGCThreshold(rt, sc->fin == 1 ? 0 : SIZE_MAX / 2);
    g_finalizer_allocs = sc->fin;

    for (int i = 0; i < iters; i++) {
        JSValue a = JS_NewInt32(ctx, i);
        JSValue v = JS_Call(ctx, step, global, 1, &a);
        CHECK(!JS_IsException(v));
        JS_ToInt32(ctx, &sum, v);
        JS_FreeValue(ctx, v);
        if (sc->fin == 2) {
            JSValue live;
            for (int k = 0; k < 8; k++)
                make_fin_cycle(JS_UNDEFINED, k);
            run_minor(rt);
            make_pending_tree(rt);
            run_minor(rt);
            /* a young object for the finalizer check */
            live = JS_NewObject(ctx);
            g_in_pending = 1;
            while (JS_FreePendingObjects(rt, 3)) {}
            g_in_pending = 0;
            JS_FreeValue(ctx, live);
            continue;
        }
        if (sc->fin) {
            for (int k = 0; k < 8; k++)
                make_fin_cycle(i % 4 == 0 ? holder : JS_UNDEFINED, i * 8 + k);
            if (i % 5 == 4) {
                /* drop what the old holder kept */
                JS_FreeValue(ctx, holder);
                holder = JS_NewArray(ctx);
            }
        }
        if (i % 4 == 0)
            JS_PromoteGCObjects(rt); /* make 'holder' old */
        JS_FreePendingObjects(rt, 7);
        if (i % 10 == 9)
            JS_RunGCMinor(rt);
    }

    g_finalizer_allocs = 0;
    JS_SetGCYoungLimit(rt, 0, NULL);
    JS_SetGCThreshold(rt, 256 * 1024);
    JS_FreeValue(ctx, holder);
    JS_FreeValue(ctx, anchor);
    JS_RunGC(rt);
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &u);
    uint32_t young, pending;
    JS_GetGCCounts(rt, &young, &pending);

    CHECK(g_hook_depth == 0);
    if (sc->hook) {
        CHECK(g_hook_begin > 0);
        CHECK(g_hook_begin == g_hook_end);
        /* hooks never run inside a hook */
        CHECK(g_hook_max_depth == 1);
    }
    if (sc->fin)
        CHECK(g_finalized > 0);
    if (sc->fin == 2) {
        CHECK(g_fin_in_minor > 0);
        CHECK(g_fin_in_pending > 0);
    }
    CHECK(pending == 0);

    printf("%s: checksum=%d hooks=%d/%d max_depth=%d finalized=%d "
           "(in minor %d, in pending %d) obj=%lld\n",
           sc->name, sum, g_hook_begin, g_hook_end, g_hook_max_depth,
           g_finalized, g_fin_in_minor, g_fin_in_pending, (long long)u.obj_count);

    JS_FreeValue(ctx, step);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return g_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    size_t n = sizeof(scenarios) / sizeof(scenarios[0]);
    if (argc < 2) {
        for (size_t i = 0; i < n; i++)
            printf("%s\n", scenarios[i].name);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(argv[1], scenarios[i].name) == 0)
            return run(&scenarios[i]);
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
