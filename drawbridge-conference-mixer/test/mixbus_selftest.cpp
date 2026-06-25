// Self-test: verifies the two correctness-critical properties of the mix bus —
// (1) minus-self (a talker never hears itself), and
// (2) the running mix is NOT pre-saturated (loud legs don't cancel via clipping).
#include "MixBus.hpp"
#include "mix_kernels.h"
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<int16_t> constFrame(int16_t v){ return std::vector<int16_t>(MixBus::FRAME, v); }

int main()
{
    MixBus bus;

    // Three legs.
    int A = bus.attach();
    int B = bus.attach();
    int C = bus.attach();
    assert(A==0 && B==1 && C==2);
    assert(bus.activePorts()==3);

    // Each leg pushes a DC frame: A=+1000, B=+2000, C=+4000.
    auto a = constFrame(1000), b = constFrame(2000), c = constFrame(4000);
    bus.inputFrame(A, a.data(), a.size());
    bus.inputFrame(B, b.data(), b.size());
    bus.inputFrame(C, c.data(), c.size());

    bus.tick();   // mix = 7000; A->6000, B->5000, C->3000

    int16_t oa[MixBus::FRAME], ob[MixBus::FRAME], oc[MixBus::FRAME];
    bool ra = bus.outputFrame(A, oa, MixBus::FRAME);
    bool rb = bus.outputFrame(B, ob, MixBus::FRAME);
    bool rc = bus.outputFrame(C, oc, MixBus::FRAME);
    assert(ra && rb && rc);
    assert(oa[0]==6000 && ob[0]==5000 && oc[0]==3000);   // minus-self correct
    printf("minus-self: A=%d B=%d C=%d  (expect 6000/5000/3000) OK\n", oa[0], ob[0], oc[0]);

    // No-over-saturation: four loud legs at +30000. Naive int16 mix would clip the
    // running sum to 32767, then C would hear 32767-30000=2767 (WRONG). Correct
    // int32 mix = 120000; C hears 120000-30000 = 90000 -> sat16 -> 32767.
    MixBus bus2;
    int p0=bus2.attach(), p1=bus2.attach(), p2=bus2.attach(), p3=bus2.attach();
    auto loud = constFrame(30000);
    bus2.inputFrame(p0, loud.data(), loud.size());
    bus2.inputFrame(p1, loud.data(), loud.size());
    bus2.inputFrame(p2, loud.data(), loud.size());
    bus2.inputFrame(p3, loud.data(), loud.size());
    bus2.tick();
    int16_t o0[MixBus::FRAME];
    bus2.outputFrame(p0, o0, MixBus::FRAME);
    // mix=120000, minus self 30000 = 90000 -> saturates to 32767, NOT 2767.
    assert(o0[0]==32767);
    printf("no-over-saturation: loud port hears %d (expect 32767, NOT 2767) OK\n", o0[0]);

    // Detach reclamation: detach B, tick reclaims, slot returns Free and re-attaches.
    bus.detach(B);
    bus.tick();                       // reclaims B -> Free
    assert(bus.activePorts()==2);
    int D = bus.attach();             // should reuse the freed slot index 1
    assert(D==B);
    printf("detach/reclaim: freed slot %d re-attached as %d OK\n", B, D);

    // Small-conference confirmed-ISA primitive: sat(a+b+c+d).
    int16_t out[MixBus::FRAME];
    auto z = constFrame(0);
    mix_sum4_s16(out, a.data(), b.data(), c.data(), z.data(), MixBus::FRAME);
    assert(out[0]==7000);             // 1000+2000+4000+0
    printf("mix_sum4_s16: %d (expect 7000) OK\n", out[0]);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
