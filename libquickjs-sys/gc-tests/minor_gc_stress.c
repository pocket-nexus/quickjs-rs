// Stress test for JS_RunGCMinor: run a cycle-heavy workload with a given GC
// schedule and print object counts after a final full GC plus a checksum.
// run.sh checks that every schedule ends with the counts of mode 0.
//
//   minor_gc_stress <mode> [iterations]
//   mode 0 = full GC only, 1 = minor GC + a full one every 200 steps,
//   2 = minor GC only (then a final full GC)
//   env: YOUNG=<young limit>, BUDGET=<pending objects freed per step>,
//        EVERY=<steps between collections>, TWICE=1 (two final full GCs)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static const char *SRC =
"var keep = []; var wm = new WeakMap(); var sum = 0; var gen = 0;\n"
"function mkCycle(n){ var a={n:n}; var b={a:a}; a.b=b; a.f=function(){return a.n+b.a.n;}; return a; }\n"
"async function af(x){ await null; return x+1; }\n"
"function* g(){ var o={}; o.self=o; yield o; }\n"
"globalThis.step = function(i){\n"
"  gen++;\n"
"  for (var k=0;k<200;k++){ var c=mkCycle(k+i); sum=(sum+c.f())|0; if(k%17==0) keep.push(c); }\n"
"  if (keep.length > 300) { var old = keep.splice(0, 150); old[0].back = keep[0]; keep[0].fwd = old; }\n"
"  if (i%7==0) { var o={}; wm.set(o, mkCycle(i)); wm.set(keep[keep.length-1], {x: keep[0]}); }\n"
"  if (i%11==0) { var it=g(); it.next(); af(i).then(function(v){ sum=(sum+v)|0; }); }\n"
"  if (i%13==0) { var m=new Map(); var s=new Set(); m.set(s,m); s.add(m); s.add(keep[0]); keep[1].m = m; }\n"
"  if (i%29==0) { keep.length = Math.min(keep.length, 40); }\n"
"  if (i%31==0) { var arr=[]; arr.push(arr); arr.push(keep[2]); keep[2].arr=arr; }\n"
"  if (i%5==0) { (function(){ var x={}; var cl=function(){return x;}; x.cl=cl; keep.push(x); })(); }\n"
"  if (i%50==0) { globalThis.T = {kids: Array.from({length:300}, function(_, j){ var k={j:j, s:'x'+j}; if (j%7==0) k.self=k; return k; })}; var wr = new WeakMap(); wr.set(globalThis.T.kids[3], 1); globalThis.WM = wr; }\n"
"  if (i%50==10) { var c = mkCycle(i); c.tree = globalThis.T; c.f2 = function(){ return c.tree; }; globalThis.T = null; globalThis.C = c; }\n"
"  if (i%50==12) { sum = (sum + globalThis.C.tree.kids.length)|0; globalThis.C = null; }\n"
"  return sum;\n"
"};\n";

int main(int argc, char **argv) {
  int mode = atoi(argv[1]); // 0 = full GC, 1 = minor GC + rare full, 2 = minor only + final full
  int iters = argc > 2 ? atoi(argv[2]) : 2000;
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue r = JS_Eval(ctx, SRC, strlen(SRC), "<t>", 0);
  if (JS_IsException(r)) { printf("eval exc\n"); return 1; }
  JS_FreeValue(ctx, r);
  if (mode != 0) JS_PromoteGCObjects(rt);
  if (mode != 0 && getenv("YOUNG")) JS_SetGCYoungLimit(rt, atoi(getenv("YOUNG")), NULL);
  JSValue g = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, g, "step");
  int32_t last = 0; JSMemoryUsage u; long maxobj = 0;
  for (int i = 0; i < iters; i++) {
    JSValue a = JS_NewInt32(ctx, i);
    JSValue v = JS_Call(ctx, fn, g, 1, &a);
    if (JS_IsException(v)) { printf("exc at %d\n", i); return 1; }
    JS_ToInt32(ctx, &last, v); JS_FreeValue(ctx, v);
    JSContext *c1; while (JS_ExecutePendingJob(rt, &c1) > 0) {}
    if (mode != 0) JS_FreePendingObjects(rt, getenv("BUDGET") ? atoi(getenv("BUDGET")) : 50);
    if (i % (getenv("EVERY") ? atoi(getenv("EVERY")) : 10) == 0) {
      if (mode == 0) JS_RunGC(rt);
      else if (mode == 1 && i % 200 == 199) JS_RunGC(rt);
      else JS_RunGCMinor(rt);
      JS_ComputeMemoryUsage(rt, &u); if (u.obj_count > maxobj) maxobj = u.obj_count;
    }
  }
  JS_RunGC(rt);
  if (getenv("TWICE")) JS_RunGC(rt);
  JS_ComputeMemoryUsage(rt, &u);
  printf("mode=%d checksum=%d obj=%lld prop=%lld shape=%lld mem=%lld maxobj=%ld\n", mode, last,
         (long long)u.obj_count, (long long)u.prop_count, (long long)u.shape_count,
         (long long)u.memory_used_size, maxobj);
  JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  printf("freed ok\n");
  return 0;
}
