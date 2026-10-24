#pragma once
/* Stage 8: UFS host probe for rosemary. do_init=1 allows enabling the host and
 * link startup if LK left it disabled; gpt_lun is the LUN that holds the GPT. */
void ufs_stage8(int do_init, int gpt_lun);
