#version 450
/* probe19: VS, который действительно читает push-константы.
 *
 * Это не украшение: ir3 заводит const_state.push_consts только когда шейдер
 * к ним обращается, а без них у пайплайна нет ни общего preamble, ни
 * регистра, который токен PUSH_CONSTANT должен патчить. Пайплайн с объявленным
 * диапазоном, но без обращения, для DGC бесполезен.
 *
 * magic != 0  → треугольник остаётся на месте;
 * magic == 0  → уезжает за экран. Так проверка пикселя доказывает, что значение
 * пришло из последовательности (или из vkCmdPushConstants), а не совпало. */
layout(location = 0) in vec2 p;

layout(push_constant) uniform Pc {
   uint magic;
   uint scale;
} pc;

void main()
{
   float shift = (pc.magic != 0u) ? 0.0 : 2.5;
   gl_Position = vec4(p.x + shift, p.y, 0.0, 1.0);
}
