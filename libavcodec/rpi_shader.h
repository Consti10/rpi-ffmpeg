#ifndef rpi_shader_H
#define rpi_shader_H

extern unsigned int rpi_shader[];

#define mc_setup_uv (rpi_shader + 0)
#define mc_filter_uv (rpi_shader + 150)
#define mc_filter_uv_b0 (rpi_shader + 368)
#define mc_filter_uv_b (rpi_shader + 586)
#define mc_exit (rpi_shader + 818)
#define mc_interrupt_exit8 (rpi_shader + 836)
#define mc_end (rpi_shader + 866)

#endif
