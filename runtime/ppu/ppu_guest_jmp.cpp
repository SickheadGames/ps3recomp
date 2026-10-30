/*
 * ps3recomp - guest setjmp/longjmp between stacks (coroutines) on host fibers
 *
 * A title that builds coroutines out of setjmp/longjmp switches stacks with a
 * longjmp: the jump buffer holds another stack's r1 and a return address in
 * code that is suspended in the middle of a call chain. On hardware that is
 * two register loads. In lifted code it is not: the lifted longjmp loads the
 * target registers into the ppu_context and then RETURNS, still on the host
 * stack of the coroutine that called it, and every lifted epilogue after that
 * pops a frame of the wrong guest stack.
 *
 * AC1's frame-job scheduler (Bink movie playback, among others) is this:
 *   spawn  (0xCF1694): setjmp(co+0x30), then overwrite r1/r2/lr in the buffer
 *                      with a fresh 0x8000-byte stack and an entry function;
 *   switch (0xCF0BBC): flag = 1; setjmp(cur+0x30);
 *                      if (flag) { flag = 0; longjmp(next+0x30, next); }
 *                      return 0;
 * and TaskThread2 came back from the switch with r1 pointing into another
 * coroutine's stack (+0x8270), restored r1 = 0 a few frames later and walked
 * into the null page at the end of demointro.bik.
 *
 * Here every coroutine runs on its own host fiber:
 *   - setjmp records which host fiber saved the buffer, and the r1/lr it saved;
 *   - longjmp to a buffer saved by ANOTHER fiber (r1/lr unchanged since) is a
 *     SwitchToFiber to it. That fiber is parked inside its own longjmp; when
 *     resumed it puts back the register state it had at that call and returns
 *     normally. That is only the same thing as its setjmp returning a second
 *     time because, in the switch pattern above, the code after the longjmp
 *     call and the code after the setjmp's second return are the same "return
 *     0" -- the switch function's own epilogue;
 *   - longjmp to a buffer whose r1/lr were rewritten after the setjmp (a spawn)
 *     starts a new fiber at the buffer's lr with the loaded registers.
 * A same-fiber longjmp (a plain non-local exit) is not handled and keeps the
 * old behaviour, loudly.
 *
 * The lifted bodies call in here (hacks.md #10): ppu_gjmp_setjmp() after the
 * save, ppu_gjmp_longjmp_begin() before the loads, ppu_gjmp_longjmp_switch()
 * after them. Windows only for now; elsewhere the hooks are no-ops.
 */
#include "ppu_context.h"
#include "ps3emu/logging.h"
#include <stdint.h>
#include <string.h>

extern "C" uint8_t* vm_base;
extern "C" void ps3_indirect_call(ppu_context* ctx);
/* ps3_indirect_call's per-thread nesting counters (ppu_loader.cpp). */
extern "C" int* ps3_ppu_dispatch_depth_slot(void);
extern "C" int* ps3_ppu_vcall_sp_slot(void);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mutex>
#include <vector>

/* Host stack of a coroutine fiber: the host frames of the lifted code it runs
 * (same size cellFiber uses). */
#define GJMP_HOST_STACK (4u * 1024u * 1024u)

namespace {

struct Buf {                /* one guest jump buffer (16-aligned EA) */
    uint32_t ea;
    void*    owner;         /* host fiber whose setjmp filled it */
    uint64_t r1, lr;        /* what that setjmp saved */
    void*    created;       /* fiber started from it by a spawn longjmp */
};
struct Co {                 /* one host fiber running guest code */
    void*       fiber;
    ppu_context snap;       /* guest state at its last outgoing longjmp */
};
struct Start {
    ppu_context* ctx;
    ppu_context  regs;      /* registers the spawning longjmp loaded */
    uint32_t     ea;
};

std::mutex          s_mu;
std::vector<Buf>    s_bufs;
std::vector<Co*>    s_cos;

PPU_THREAD_LOCAL ppu_context* t_pending_ctx;   /* begin() -> switch() handoff */
PPU_THREAD_LOCAL ppu_context  t_pending;
PPU_THREAD_LOCAL void*        t_home;          /* the thread's own fiber */

uint64_t be64(uint32_t ea)
{
    uint64_t v; memcpy(&v, vm_base + ea, 8);
    return _byteswap_uint64(v);
}

void* cur_fiber()
{
    if (!IsThreadAFiber()) ConvertThreadToFiber(nullptr);
    void* f = GetCurrentFiber();
    if (!t_home) t_home = f;
    return f;
}

Buf* find_buf(uint32_t ea)          /* s_mu held */
{
    for (Buf& b : s_bufs) if (b.ea == ea) return &b;
    return nullptr;
}

Co* co_of(void* f)                  /* s_mu held */
{
    for (Co* c : s_cos) if (c->fiber == f) return c;
    Co* c = new Co();
    c->fiber = f;
    s_cos.push_back(c);
    return c;
}

/* Forget a fiber that will never run again and free its host stack. */
void drop_fiber(void* f)            /* s_mu held; f is not the running fiber */
{
    for (Buf& b : s_bufs) {
        if (b.owner == f)   b.owner = nullptr;
        if (b.created == f) b.created = nullptr;
    }
    for (size_t i = 0; i < s_cos.size(); i++)
        if (s_cos[i]->fiber == f) { delete s_cos[i]; s_cos.erase(s_cos.begin() + i); break; }
    DeleteFiber(f);
}

VOID CALLBACK co_entry(void* p)
{
    Start* st = (Start*)p;
    ppu_context* ctx = st->ctx;
    const uint32_t ea = st->ea;
    *ctx = st->regs;
    delete st;
    const uint32_t entry = (uint32_t)ctx->lr;
    /* A fresh host stack: no ps3_indirect_call frames on it yet. */
    *ps3_ppu_dispatch_depth_slot() = 0;
    *ps3_ppu_vcall_sp_slot() = 0;
    ps3_log("[gjmp] tid=%u coroutine start buf=0x%08X entry=0x%08X r1=0x%08X\n",
            (unsigned)ctx->thread_id, ea, entry, (uint32_t)ctx->gpr[1]);
    ctx->ctr = entry;
    ctx->lr  = 0;
    ps3_indirect_call(ctx);
    /* A coroutine leaves by switching away, never by returning: there is no
     * guest frame to return to. Park on the thread's own fiber. */
    ps3_log("[gjmp] tid=%u coroutine entry 0x%08X RETURNED (r3=0x%08X) -- parking\n",
            (unsigned)ctx->thread_id, entry, (uint32_t)ctx->gpr[3]);
    for (;;) SwitchToFiber(t_home);
}

} // namespace

extern "C" void ppu_gjmp_setjmp(ppu_context* ctx, uint32_t ea)
{
    (void)ctx;
    void* f = cur_fiber();
    std::lock_guard<std::mutex> lk(s_mu);
    Buf* b = find_buf(ea);
    if (!b) { s_bufs.push_back(Buf{ ea, nullptr, 0, 0, nullptr }); b = &s_bufs.back(); }
    b->owner = f;
    b->r1    = be64(ea + 0x00);
    b->lr    = be64(ea + 0xA8);
}

extern "C" void ppu_gjmp_longjmp_begin(ppu_context* ctx)
{
    t_pending     = *ctx;
    t_pending_ctx = ctx;
}

extern "C" void ppu_gjmp_longjmp_switch(ppu_context* ctx, uint32_t ea)
{
    void* me = cur_fiber();
    void* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        co_of(me)->snap = (t_pending_ctx == ctx) ? t_pending : *ctx;
        t_pending_ctx = nullptr;

        Buf* b = find_buf(ea);
        const uint64_t r1 = be64(ea + 0x00), lr = be64(ea + 0xA8);
        const bool intact = b && b->owner && b->r1 == r1 && b->lr == lr;
        if (intact && b->owner == me) {
            static int s_n = 0;
            if (s_n++ < 8)
                ps3_log("[gjmp] tid=%u same-fiber longjmp buf=0x%08X lr=0x%08X -- NOT HANDLED, "
                        "registers loaded and returning as before\n",
                        (unsigned)ctx->thread_id, ea, (uint32_t)lr);
            return;
        }
        if (intact) {
            target = b->owner;
            static long s_n = 0;
            if (s_n < 256 || (s_n & 4095) == 0)
                ps3_log("[gjmp] tid=%u switch #%ld -> buf=0x%08X (resume lr=0x%08X r1=0x%08X)\n",
                        (unsigned)ctx->thread_id, s_n, ea, (uint32_t)lr, (uint32_t)r1);
            s_n++;
        } else {
            if (!b) { s_bufs.push_back(Buf{ ea, nullptr, 0, 0, nullptr }); b = &s_bufs.back(); }
            /* The buffer is being reused for a new coroutine, so the one last
             * started from it has finished with it. */
            if (b->created && b->created != me && b->created != t_home) drop_fiber(b->created);
            Start* st = new Start();
            st->ctx = ctx; st->regs = *ctx; st->ea = ea;
            target = CreateFiberEx(64 * 1024, GJMP_HOST_STACK, 0, co_entry, st);
            if (!target) {
                delete st;
                ps3_log("[gjmp] CreateFiberEx failed (%lu) -- returning as before\n", GetLastError());
                return;
            }
            b->owner = nullptr;
            b->created = target;
        }
    }

    /* ps3_indirect_call counts its nesting per host thread, but a fiber that
     * switches away leaves its frames unwound, so a thread-wide count only
     * grows. AC1 spawns ~3 coroutines a frame; at depth 4000 the guard began
     * skipping every indirect call on that worker, the frame's job graph
     * never finished and the title/menu froze (~35 s in). Each fiber keeps
     * its own count: saved here on its own stack, restored on resume (on
     * whichever thread resumes it). */
    const int my_depth = *ps3_ppu_dispatch_depth_slot();
    const int my_vsp   = *ps3_ppu_vcall_sp_slot();

    SwitchToFiber(target);

    *ps3_ppu_dispatch_depth_slot() = my_depth;
    *ps3_ppu_vcall_sp_slot()       = my_vsp;

    /* Resumed: another coroutine longjmp'd to the buffer this fiber saved. Its
     * lifted longjmp loaded our setjmp-time registers and the value in r3;
     * carry on from our own call with the state we left it in. */
    const uint64_t r3 = ctx->gpr[3];
    {
        std::lock_guard<std::mutex> lk(s_mu);
        *ctx = co_of(me)->snap;
    }
    ctx->gpr[3] = r3;
}

#else  /* !_WIN32: not implemented, keep the old behaviour */

extern "C" void ppu_gjmp_setjmp(ppu_context*, uint32_t) {}
extern "C" void ppu_gjmp_longjmp_begin(ppu_context*) {}
extern "C" void ppu_gjmp_longjmp_switch(ppu_context*, uint32_t) {}

#endif
