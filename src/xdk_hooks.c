#include <stdint.h>

typedef void (*xdk_fn)(void);
extern void sub_001DA850_gen(void);
extern void xdk_hook_d3d_block_on_time(xdk_fn original, uint32_t guest_pc);
extern void sub_001EBAD7_gen(void);
extern void sub_001EBC73_gen(void);
extern void xdk_hook_dsound_gp_parse_effect_image(xdk_fn original, uint32_t guest_pc);
extern void xdk_hook_dsound_gp_commit_changes(xdk_fn original, uint32_t guest_pc);

/* Verified in MKDA's D3D section: loads device [0x1E4FD0], semaphore
 * pointer at device+0x34, and waits for its requested timestamp. The shared
 * hook decodes these from this title's bytes and retains the original body. */
void sub_001DA850(void)
{
    xdk_hook_d3d_block_on_time(sub_001DA850_gen, 0x001DA850);
}

/* Verified thiscall manager->8->10->scratch, command at scratch+810.
 * Parse posts command 3 and waits; Commit posts command 2 at the same word. */
void sub_001EBAD7(void)
{
    xdk_hook_dsound_gp_parse_effect_image(sub_001EBAD7_gen, 0x001EBAD7);
}

void sub_001EBC73(void)
{
    xdk_hook_dsound_gp_commit_changes(sub_001EBC73_gen, 0x001EBC73);
}
