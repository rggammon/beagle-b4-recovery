/*
 * GL_OES_vertex_array_object stand-in for DDK 1.4, which lacks the extension.
 *
 * femtovg (Slint's GLES2 renderer) unconditionally creates and binds one VAO
 * through glow, which panics when glGenVertexArraysOES is not loadable. femtovg
 * re-specifies every vertex attribute after binding, so no-op VAOs over the
 * default attribute state are equivalent for it. Not a general VAO emulation.
 *
 * Also makes glDetachShader a no-op: on DDK 1.4, detaching shaders from a
 * linked program makes later draws with that program after a render-target
 * switch drop their vertex-shader uniforms (draws come out empty) until the
 * bound vertex buffer is reallocated. Only detaching the vertex shader triggers
 * it, and re-attaching later cures it: the driver reads the program's attached
 * vertex shader at draw time. Keeping shaders attached is legal GL; they are
 * freed with the program. SGXVAO_REAL_DETACH=1 forwards to the real call (to
 * reproduce the bug). Same bug as bgfx #1105 (SGX540, DDK 1.8).
 *
 * LD_PRELOAD into the app: the hard-float shim's eglGetProcAddress resolves
 * names via dlsym(RTLD_DEFAULT) first, so these definitions are returned.
 *
 *   arm-linux-gnueabihf-gcc -O2 -shared -fPIC -o libsgxvao.so gles-oes-vao-stub.c
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>

typedef int GLsizei;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;

static GLuint next_id = 1;

void glGenVertexArraysOES(GLsizei n, GLuint *arrays)
{
	for (GLsizei i = 0; i < n; i++)
		arrays[i] = next_id++;
}

void glBindVertexArrayOES(GLuint array)
{
	(void)array;
}

void glDeleteVertexArraysOES(GLsizei n, const GLuint *arrays)
{
	(void)n;
	(void)arrays;
}

GLboolean glIsVertexArrayOES(GLuint array)
{
	return array != 0 && array < next_id;
}

void glDetachShader(GLuint program, GLuint shader)
{
	static void (*real)(GLuint, GLuint);
	static int mode = -1;

	if (mode < 0) {
		const char *e = getenv("SGXVAO_REAL_DETACH");
		mode = e && e[0] == '1';
		if (mode)
			real = (void (*)(GLuint, GLuint))dlsym(RTLD_NEXT, "glDetachShader");
	}
	if (mode && real)
		real(program, shader);
}
