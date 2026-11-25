#version 450
/* probe18: второй FS-маркер, чтобы отличить pipeline, переданный в pNext
 * execute, от того, что случайно остался связанным. */
layout(location = 0) out vec4 c;
void main() { c = vec4(1.0, 0.0, 0.0, 1.0); }  /* red */
