/*
 * GL_OES_vertex_array_object stand-in for DDK 1.4, which lacks the extension.
 *
 * femtovg (Slint's GLES2 renderer) unconditionally creates and binds one VAO
 * through glow, which panics when glGenVertexArraysOES is not loadable. femtovg
 * re-specifies every vertex attribute after binding, so no-op VAOs over the
 * default attribute state are equivalent for it. Not a general VAO emulation.
 *
 * LD_PRELOAD into the app: the hard-float shim's eglGetProcAddress resolves
 * names via dlsym(RTLD_DEFAULT) first, so these definitions are returned.
 *
 *   arm-linux-gnueabihf-gcc -O2 -shared -fPIC -o libsgxvao.so gles-oes-vao-stub.c
 */
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
