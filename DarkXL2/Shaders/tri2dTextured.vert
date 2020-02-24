uniform vec4 ScaleOffset;
in vec2 vtx_pos;
in vec2 vtx_uv;
in vec4 vtx_color;
out vec4 Frag_Color;
out vec2 Frag_Uv;
void main()
{
    vec2 pos = vtx_pos.xy * ScaleOffset.xy + ScaleOffset.zw;
    Frag_Uv = vtx_uv;
    Frag_Color = vec4(vtx_color.rgb*vtx_color.a, vtx_color.a);
    gl_Position = vec4(pos.xy, 0, 1);
}