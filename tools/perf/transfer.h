// millsaph_bench --transfer: CUDA copy speeds. See transfer.c.

#pragma once

// Times copies of a width x height RGBA float image to the GPU and back. Returns 0 on success.
int ms_transfer_bench(int width, int height);
