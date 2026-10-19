/*
 * nv2a_gl.h -- OpenGL renderer for the NV2A pushbuffer executor.
 *
 * Call nv2a_gl_install() before the title starts rendering, with the
 * executor enabled (RECOMP_PB_EXEC). The window and GL context are created
 * on the executor's thread on first use.
 */
#ifndef NV2A_GL_H
#define NV2A_GL_H

void nv2a_gl_install(void);
/* Terminal shutdown: stop all producers first and keep guest RAM alive until
 * this returns. Drains/joins the optional queue and releases the GL context. */
int nv2a_gl_shutdown(void);
void nv2a_gl_queue_drain(void);
/* Call on a context-owning producer before its thread exits. */
int nv2a_gl_detach_context(void);

#endif
