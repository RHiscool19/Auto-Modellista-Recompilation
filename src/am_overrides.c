/**
 * Auto Modellista: hand-written replacements for generated functions.
 *
 * Every function defined here as `void sub_XXXXXXXX(void)` is skipped by the
 * recompiler (scripts/regen.ps1 passes this file to --exclude-manual), so all
 * generated callers and the dispatch table reach these versions instead.
 * After adding or removing one, re-run:  .\scripts\regen.ps1 -RecompOnly
 */

#include <math.h>
#include "recomp/gen/recomp_types.h"

/* CRT sqrt (double), guest 0x001EEB84.
 *
 * Called through float wrappers like sub_001067D0. For a negative argument the
 * original takes the CRT's floating-point error path (sub_001F2347 ->
 * sub_001F789D -> sub_001F3423), which crashed in the attract demo: the guest
 * esp ended up above the top of the main stack and a write went to a garbage
 * address (0xC0B62C6E). The game ignores the error anyway, so compute the
 * result natively: NaN for a negative input, as the x87 would produce.
 *
 * Convention (from the callers): cdecl, double argument at [esp+4] above the
 * return address, result returned in st(0). */
void sub_001EEB84(void)
{
    double x = MEMD(g_esp + 4);
    double r = sqrt(x);

    g_fp_top = (g_fp_top + 7) & 7;      /* fld: push result onto the x87 stack */
    g_fp_stack[g_fp_top] = r;
    g_esp += 4;                         /* ret (caller pops the argument) */
}
